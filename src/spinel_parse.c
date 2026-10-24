/*
 * spinel_parse.c - Prism AST Serializer (C version)
 *
 * Equivalent to spinel_parse.rb but links with libprism directly.
 * Parses Ruby source and fills the compiler's NodeTable (node_table.h) as
 * it walks Prism's tree: each node gets the next id, its type and its named
 * string / int / float / ref / array fields. nt_dump prints the table in the
 * text form docs/internals/AST.md describes (--dump-ast).
 *
 * Build: linked into the compiler binary (build/csrc/sp_parse_lib.o).
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>      /* readlink: resolve the executable for gem lookup */
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#include <prism.h>
#include "node_table.h"

/* node_table.h names the node flags the passes read without including Prism;
   each must be the value Prism's own constant has. */
_Static_assert(NT_RANGE_EXCLUDE_END == PM_RANGE_FLAGS_EXCLUDE_END, "range exclude-end flag");
_Static_assert(NT_LOOP_BEGIN_MODIFIER == PM_LOOP_FLAGS_BEGIN_MODIFIER, "loop begin-modifier flag");
_Static_assert(NT_RE_IGNORE_CASE == PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE, "regexp /i flag");
_Static_assert(NT_RE_EXTENDED == PM_REGULAR_EXPRESSION_FLAGS_EXTENDED, "regexp /x flag");
_Static_assert(NT_RE_MULTI_LINE == PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE, "regexp /m flag");
_Static_assert(NT_RE_EUC_JP == PM_REGULAR_EXPRESSION_FLAGS_EUC_JP, "regexp /e flag");
_Static_assert(NT_RE_ASCII_8BIT == PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT, "regexp /n flag");
_Static_assert(NT_RE_WINDOWS_31J == PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J, "regexp /s flag");
_Static_assert(NT_RE_UTF_8 == PM_REGULAR_EXPRESSION_FLAGS_UTF_8, "regexp /u flag");

#if defined(_WIN32)
char *sp_w32_overlay(char *content, const char *path);   /* lib/win32/sp_win32_driver.c */
#endif

/* ---- Output ----
   The table the walk fills, and the id the next node it reaches gets. */
static NodeTable *out_nt;
static int node_counter;

/* ---- Name from constant pool ---- */
static const pm_parser_t *g_parser;
static const char *g_source_file = "";
/* When SPINEL_DEBUG, SPINEL_LINE_MAP or SPINEL_POSITIONS is 1, flatten()
   emits a per-node `node_line` field, which codegen places C `#line`
   directives by and the analysis reads. The compiler driver sets
   SPINEL_POSITIONS for every compile; off otherwise, so the AST text format
   (and golden tests) are unchanged. */
static int g_emit_line = 0;
/* The buffer-line -> (file, line) map is built for every program, not only
   under g_emit_line: `__FILE__` and `__dir__` in a required file answer
   that file, not the entry script (#4839), and only the map knows which file
   a node came from once the requires are spliced into one buffer. The
   PUSH/POP marker lines it is rebuilt from are comments. g_emit_line still
   decides whether nodes carry their line for `#line` directives. */
static int g_src_map = 1;
/* --emit-types (SPINEL_EMIT_TYPES) also gets each node's END position, so a
   consumer can pick the tightest span under a cursor: `pts`, `pts.map { }`
   and `.inspect` all start at one column. Only then: two more attributes on
   every node are node-table fields the ordinary compile has no use for (#4522). */
static int g_emit_end = 0;
/* Per-line frozen_string_literal flags for the final spliced buffer (0-based
   line index): the require resolvers build it, one byte per line, from each
   spliced file's own flag (sp_src_file_fsl), and flatten() stamps an `fzl`
   field on string-literal nodes from it. */
static unsigned char *g_fsl_lines = NULL;
static size_t g_fsl_nlines = 0;

/* ---- Prism, told Spinel's policy ----
   Every Prism parser the front end makes is created here (#9898), with the
   options a CRuby 4.0 run of the program has: string literals frozen
   (Spinel has no chilled mode; see docs/limitations.md), the 4.0 grammar
   (with no options Prism parses as its latest version), the file's path
   when the text is one file's (NULL for a buffer of several, or a
   class_eval string; a hoisted file's, with what it required inlined, is
   that file's). Prism keeps `path` (not a copy) for the parser's life.
   No parser is the main script, the entry file's included: in Prism 1.9.0
   that flag makes a main script whose first line is a shebang not naming
   ruby (`#!/usr/bin/env spinel`) a search for one that does, as `ruby -x`
   would, and a parse error when there is none, which Spinel, compiling the
   file as written, must not take (the dead-require fold reads that parse).
   It would buy nothing else here: Spinel reads no shebang options. */
static void sp_pm_parser_init(pm_parser_t *parser, const char *src, size_t len,
                              const char *path) {
  pm_options_t opts;
  memset(&opts, 0, sizeof opts);
  pm_options_line_set(&opts, 1);
  pm_options_frozen_string_literal_set(&opts, true);
  pm_options_version_set(&opts, "4.0", 3);
  if (path) pm_options_filepath_set(&opts, path);
  pm_parser_init(parser, (const uint8_t *)src, len, &opts);
  pm_options_free(&opts);
}

/* One file's text parsed by itself, before anything is spliced into it: the
   facts of the file (its frozen_string_literal pragma, its source encoding,
   the requires its dead branches hold) are read from this parse. */
typedef struct {
  const char *path;
  const char *text;
  pm_parser_t parser;
  pm_node_t *root;
} SpSrcFile;

static void sp_src_file_parse(SpSrcFile *f, const char *text, const char *path) {
  f->path = path;
  f->text = text;
  sp_pm_parser_init(&f->parser, text, strlen(text), path);
  f->root = pm_parse(&f->parser);
}

static void sp_src_file_free(SpSrcFile *f) {
  pm_node_destroy(&f->parser, f->root);
  pm_parser_free(&f->parser);
}

/* Does the magic comment say `frozen_string_literal: false`? Prism lists
   every `key: value` comment as written; it takes a key case-insensitively,
   with `-` for `_`, and the value case-insensitively. */
static bool sp_magic_comment_fsl_false(const pm_magic_comment_t *mc) {
  static const char key[] = "frozen_string_literal";
  if (mc->key_length != sizeof key - 1 || mc->value_length != 5) return false;
  for (uint32_t i = 0; i < mc->key_length; i++) {
    char ch = (char)tolower(mc->key_start[i]);
    if ((ch == '-' ? '_' : ch) != key[i]) return false;
  }
  for (uint32_t i = 0; i < 5; i++)
    if (tolower(mc->value_start[i]) != "false"[i]) return false;
  return true;
}

/* The frozen_string_literal flag of the file's literals: 1, in every file.
   Prism reads the pragma wherever CRuby does (any comment ahead of the
   first token) and starts from Spinel's frozen default, so it answers
   DISABLED only for a `false` pragma. Spinel has no chilled mode (the
   mutable-string machinery needs the whole-program guarantee; see
   docs/limitations.md): an explicit opt-out is ignored loudly rather than
   honored partially, with a warning at the pragma's line each time the
   file is read. */
static int sp_src_file_fsl(const SpSrcFile *f) {
  if (f->parser.frozen_string_literal != PM_OPTIONS_FROZEN_STRING_LITERAL_DISABLED) return 1;
  int32_t line = 1;
  for (const pm_list_node_t *n = f->parser.magic_comment_list.head; n; n = n->next) {
    const pm_magic_comment_t *mc = (const pm_magic_comment_t *)n;
    if (sp_magic_comment_fsl_false(mc)) {
      line = pm_newline_list_line(&f->parser.newline_list, mc->key_start, f->parser.start_line);
      break;
    }
  }
  fprintf(stderr,
          "spinel: %s:%d: warning: frozen_string_literal: false is not supported "
          "in Spinel (string literals are always frozen); the pragma is "
          "ignored\n", f->path, (int)line);
  return 1;
}

/* How much of a file's text is code: up to its `__END__` line, which
   Prism found (data_loc), or all of it. Nothing after that line is code.
   In a required file, spliced into the buffer as text, it ended Prism's
   reading of the whole program there: every file spliced after it and the
   rest of the program were dropped without a word (#9898); in any file the
   require scanners read it as code. The entry file is cut the same way:
   DATA, which would read its data section, is still not defined (a
   compile-time warning, then NameError when reached), as on master; it is
   left for later. The length is the parser's, which saw the whole text. */
static size_t sp_src_file_code_len(const SpSrcFile *f) {
  const uint8_t *end = f->parser.data_loc.start ? f->parser.data_loc.start : f->parser.end;
  return (size_t)(end - f->parser.start);
}

/* Spinel's Strings are UTF-8 (#9898). A magic comment naming another
   encoding is honoured where Prism reads the text (a byte that encoding
   allows in a literal parses), and each literal keeps the bytes it is
   written with, but every String, literals included, takes UTF-8 as its
   encoding, and so does `__ENCODING__`, where CRuby gives them the
   comment's. The warning at the comment's line says so. */
static void sp_src_file_warn_encoding(const SpSrcFile *f) {
  if (f->parser.encoding == PM_ENCODING_UTF_8_ENTRY) return;
  int32_t line = pm_newline_list_line(&f->parser.newline_list, f->parser.encoding_comment_start,
                                      f->parser.start_line);
  fprintf(stderr,
          "spinel: %s:%d: warning: source encoding %s: Spinel's Strings are "
          "UTF-8, so this file's literals keep the bytes they are written with "
          "but take UTF-8 as their encoding\n", f->path, (int)line, f->parser.encoding->name);
}

/* Lines of `s` as the require splicers count them: one per '\n', plus one for
   a trailing unterminated fragment (the splicers append the missing '\n'). */
static size_t sp_count_lines(const char *s) {
  size_t n = 0; const char *p = s;
  for (; *p; p++) if (*p == '\n') n++;
  if (p != s && p[-1] != '\n') n++;
  return n;
}

/* Fresh flag buffer for `text`, every line carrying that file's pragma flag. */
static unsigned char *sp_fsl_make(const char *text, int flag, size_t *n_out) {
  size_t n = sp_count_lines(text);
  unsigned char *v = malloc(n ? n : 1);
  if (!v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memset(v, flag ? 1 : 0, n);
  *n_out = n;
  return v;
}

/* Replace `remove` entries at line index `at` with the `insn` entries of
   `ins`, mirroring a text splice on the flag buffer. */
static void sp_fsl_splice(unsigned char **buf, size_t *n, size_t at,
                          size_t remove, const unsigned char *ins, size_t insn) {
  if (at > *n) at = *n;
  if (remove > *n - at) remove = *n - at;
  size_t newn = *n - remove + insn;
  unsigned char *nv = malloc(newn ? newn : 1);
  if (!nv) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (at) memcpy(nv, *buf, at);
  if (insn) memcpy(nv + at, ins, insn);
  size_t rest = *n - at - remove;
  if (rest) memcpy(nv + at + insn, *buf + at + remove, rest);
  free(*buf);
  *buf = nv;
  *n = newn;
}
/* Debug multi-file source map, populated by sp_build_line_map() before
   flatten() runs and read in flatten() to attribute each node to its
   original file/line. Declared here because flatten() precedes the
   require-resolution machinery that defines the builder. */
static int *sp_line_file = NULL;  /* buffer line (1-based) -> file id */
static int *sp_line_orig = NULL;  /* buffer line (1-based) -> original line */
static int *sp_line_pop = NULL;
/* The position a `#<SPINEL_SOURCE>file:line` marker pins for the lines after
   it (0 = none). Kept apart from the physical map above: `__FILE__`,
   `__dir__` and `require_relative` still answer from the file the code is in,
   only the positions handed to `#line`, debug and the reports follow it. */
static int *sp_disp_file = NULL;
static int *sp_disp_line = NULL;
static pm_node_t **g_stmt_next, **g_stmt_end;
static const uint8_t *g_owner;
static int sp_line_map_n = 0;
static char **sp_file_table;   /* id -> path; defined with the map builder below */

/* The file a node was written in when that is a required file, or NULL for
   the entry script (file id 0) and when the map could not be built. */
static const char *sp_node_required_file(const pm_node_t *node) {
  if (sp_line_map_n <= 0) return NULL;
  pm_line_column_t lc = pm_newline_list_line_column(&g_parser->newline_list,
                                                    node->location.start,
                                                    g_parser->start_line);
  if (lc.line < 1 || lc.line > sp_line_map_n || sp_line_orig[lc.line] <= 0) return NULL;
  int fid = sp_line_file[lc.line];
  return fid > 0 ? sp_file_table[fid] : NULL;
}

/* What `__FILE__` answers in a required file: its absolute path, as CRuby
   gives for require_relative and for a -I load path. Caller frees. */
static char *sp_required_file_path(const char *path) {
  char *abs = realpath(path, NULL);
  return abs ? abs : strdup(path);
}

static char *cstr(pm_constant_id_t id) {
  if (id == 0) return strdup("");
  pm_constant_t *c = &g_parser->constant_pool.constants[id - 1];
  char *buf = malloc(c->length + 1);
  memcpy(buf, c->start, c->length);
  buf[c->length] = '\0';
  return buf;
}

static void sp_find_builtin_ranges(const char *src);
static int sp_in_builtin(const uint8_t *at);
#include "sp_macro.c"
#include "sp_const_loop.c"

/* `s` as the body of a JSON string: the two escapes JSON requires and the
   control characters, for the parse-error diagnostics --emit-types writes
   before codegen (whose own writer) exists. */
static void json_fputs(const char *s, FILE *f) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
    else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
    else fputc(*p, f);
  }
}

/* Convert "PM_FOO_BAR_NODE" -> "FooBarNode" into `out`, truncated to
   out_size-1 chars + NUL. Prism's node-type strings are pure ASCII
   upper + underscore so we add 32 to lowercase non-leading letters. */
static size_t prism_kind_to_pascal(const char *raw, char *out, size_t out_size) {
  if (out_size == 0) return 0;
  if (strncmp(raw, "PM_", 3) == 0) raw += 3;
  size_t j = 0;
  int upper = 1;
  for (; *raw && j < out_size - 1; raw++) {
    if (*raw == '_') { upper = 1; continue; }
    out[j++] = upper ? *raw : (char)(*raw + 32);
    upper = 0;
  }
  out[j] = '\0';
  return j;
}

/* ---- Forward ---- */
static int flatten(pm_node_t *node);
/* inline RBS (inline_rbs.c): the facts of a node, written by flatten */
static void sp_inline_rbs_emit(const pm_node_t *node, int id);
/* --no-inline-rbs: the driver clears it, and comments are only comments */
int sp_inline_rbs_enabled = 1;
/* set when an inline RBS error, already reported, stopped the parse */
int sp_inline_rbs_failed = 0;
static int sp_in_builtin(const uint8_t *at);   /* a builtins/ splice (below) */

/* ---- Emit helpers ---- */
static void emit_bytes(int id, const char *field, const char *val, size_t len) {
  nt_put_str(out_nt, id, field, val, len);
}

static void emit_str(int id, const char *field, const char *val) {
  nt_put_str(out_nt, id, field, val, strlen(val));
}

static void emit_pm_string(int id, const char *field, const pm_string_t *s) {
  nt_put_str(out_nt, id, field, (const char *)pm_string_source(s), pm_string_length(s));
}

static void emit_int(int id, const char *field, long long val) {
  nt_put_int(out_nt, id, field, val);
}

static void emit_float(int id, double val) {
  /* Issue #766: 64-byte buf; "%.17g" produces at most 24 chars, plus
     ".0" trailer = 26. Safe by margin.
     Issue #767: snprintf is locale-sensitive (de_DE produces "3,14"
     for 3.14, which then fails to parse as a C float literal).
     Format into a fresh sprintf via the C locale by using a manual
     dot conversion: produce in current locale then replace ',' -> '.'.
     The lib-side replacement is safe because Ruby float literals never
     contain a comma. */
  char buf[64];
  /* an out-of-range literal (1e400) parses to +/-inf, which "%.17g" prints
     as "inf" -- not a C token. Emit the C99 macros instead. */
  if (isinf(val)) { nt_put_content(out_nt, id, val > 0 ? "INFINITY" : "-INFINITY"); return; }
  if (isnan(val)) { nt_put_content(out_nt, id, "NAN"); return; }
  snprintf(buf, sizeof(buf), "%.17g", val);
  for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t l = strlen(buf);
    if (l + 3 < sizeof(buf)) strcat(buf, ".0");
  }
  nt_put_content(out_nt, id, buf);
}

static void emit_ref(int id, const char *field, pm_node_t *child) {
  int cid = child ? flatten(child) : -1;
  nt_put_ref(out_nt, id, field, cid);
}

/* A block's or lambda's own locals (params + first-assigned-inside), joined
   with commas: Ruby scoping makes the non-param ones FRESH on every call, and
   a name the enclosing scope assigns only after the block's text is the
   block's own, not the enclosing one. */
static void out_block_locals(int id, const pm_constant_id_list_t *locals) {
  if (locals->size == 0) return;
  /* join the names in one pass (single cstr per name, no strcat rescans) */
  size_t total = 1;
  char **nms = malloc(locals->size * sizeof(char *));
  if (!nms) return;
  for (size_t li = 0; li < locals->size; li++) {
    nms[li] = cstr(locals->ids[li]);
    total += strlen(nms[li]) + 1;
  }
  char *joined = malloc(total);
  if (joined) {
    char *w = joined;
    for (size_t li = 0; li < locals->size; li++) {
      if (li) *w++ = ',';
      size_t nl2 = strlen(nms[li]);
      memcpy(w, nms[li], nl2); w += nl2;
    }
    *w = '\0';
    emit_str(id, "locals", joined);
    free(joined);
  }
  for (size_t li = 0; li < locals->size; li++) free(nms[li]);
  free(nms);
}

static void emit_node_array(int id, const char *field, pm_node_list_t *list) {
  if (!list || list->size == 0) {
    nt_put_arr(out_nt, id, field, NULL, 0);
    return;
  }
  int *ids = malloc(sizeof(int) * list->size);
  if (!ids) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (size_t i = 0; i < list->size; i++)
    ids[i] = flatten(list->nodes[i]);
  nt_put_arr(out_nt, id, field, ids, (int)list->size);
}

/* ---- Integer value extraction ---- */
/* The width of the TARGET's sp_int (a pointer's, lib/sp_types.h): 64 on a
   64-bit host, 32 on a 32-bit one, and whatever the C compiler named by
   --cc builds for, which main.c asks it. A literal past that width is a
   Bignum on the target, as it is in CRuby there, so it is carried as its
   decimal text (bigval) rather than truncated into the smaller sp_int. */
int sp_target_int_bits = (int)(sizeof(void *) * CHAR_BIT);
static uint64_t pm_int_max_positive(void) { return sp_target_int_bits == 32 ? (uint64_t)INT32_MAX : (uint64_t)LLONG_MAX; }
static long long pm_int_value(pm_integer_t *integer) {
  uint64_t val = 0;
  uint64_t max_positive = pm_int_max_positive();
  uint64_t max_negative = max_positive + 1ULL;
  const size_t limb_bits = 32;
  const size_t value_bits = sizeof(val) * CHAR_BIT;
  int overflow = 0;

  if (integer->values == NULL) {
    val = (uint64_t)integer->value;
  }
else {
    for (size_t i = 0; i < integer->length; i++) {
      if (i >= value_bits / limb_bits) {
        if (integer->values[i] != 0) overflow = 1;
        continue;
      }
      size_t shift = i * limb_bits;
      val |= ((uint64_t)integer->values[i]) << shift;
    }
  }

  if (integer->negative) {
    if (overflow || val >= max_negative) return -(long long)max_positive - 1;
    return -(long long)val;
  }
  if (overflow || val > max_positive) return (long long)max_positive;
  return (long long)val;
}

/* Whether an integer literal does not fit in a signed 64-bit `value` (so
   pm_int_value saturated it). Mirrors the limb scan above. */
static int pm_int_overflows(pm_integer_t *integer) {
  const size_t limb_bits = 32;
  const size_t value_bits = sizeof(uint64_t) * CHAR_BIT;
  uint64_t val = 0;
  int overflow = 0;
  /* a single-limb value (0xdeadbeef) sits in `value` with no limb array,
     and on a 32-bit target it is past the sp_int all the same */
  if (integer->values == NULL) val = (uint64_t)integer->value;
  else for (size_t i = 0; i < integer->length; i++) {
    if (i >= value_bits / limb_bits) {
      if (integer->values[i] != 0) overflow = 1;
      continue;
    }
    val |= ((uint64_t)integer->values[i]) << (i * limb_bits);
  }
  uint64_t max_positive = pm_int_max_positive();
  uint64_t max_negative = max_positive + 1ULL;
  /* -2**63 (-2**31 on a 32-bit target) is an sp_int: no bit pattern of the
     word is reserved for nil any more */
  if (integer->negative) return overflow || val > max_negative;
  return overflow || val > max_positive;
}

/* The exact decimal text of an integer literal, as CRuby prints it (the
   caller frees). pm_buffer_value is not NUL-terminated; bound by length. */
static char *pm_int_text(pm_integer_t *integer) {
  pm_buffer_t bb;
  pm_buffer_init(&bb);
  pm_integer_string(&bb, integer);
  size_t bl = pm_buffer_length(&bb);
  char *s = malloc(bl + 1);
  if (!s) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(s, pm_buffer_value(&bb), bl);
  s[bl] = '\0';
  pm_buffer_free(&bb);
  return s;
}

/* frozen_string_literal flag for the file `node`'s source line came from:
   the per-line table when available, else 1, the flag every file's
   literals get (sp_src_file_fsl). */
static int sp_node_fsl(pm_node_t *node) {
  if (g_fsl_lines) {
    int32_t bl = pm_newline_list_line(&g_parser->newline_list,
                                      node->location.start, g_parser->start_line);
    if (bl >= 1 && (size_t)bl <= g_fsl_nlines) return g_fsl_lines[bl - 1];
  }
  return 1;
}

/* `__FILE__` (1), `$0` / `$PROGRAM_NAME` (2), either one under a single
   `File.expand_path(...)` (the same code), anything else 0. */
static int sp_guard_operand(pm_node_t *node, int wrapped) {
  if (!node) return 0;
  if (PM_NODE_TYPE(node) == PM_SOURCE_FILE_NODE) return 1;
  if (PM_NODE_TYPE(node) == PM_GLOBAL_VARIABLE_READ_NODE) {
    char *gn = cstr(((pm_global_variable_read_node_t *)node)->name);
    int r = strcmp(gn, "$0") == 0 || strcmp(gn, "$PROGRAM_NAME") == 0 ? 2 : 0;
    free(gn);
    return r;
  }
  if (wrapped || PM_NODE_TYPE(node) != PM_CALL_NODE) return 0;
  pm_call_node_t *c = (pm_call_node_t *)node;
  if (!c->receiver || PM_NODE_TYPE(c->receiver) != PM_CONSTANT_READ_NODE || c->block) return 0;
  if (!c->arguments || c->arguments->arguments.size != 1) return 0;
  char *rn = cstr(((pm_constant_read_node_t *)c->receiver)->name);
  char *mn = cstr(c->name);
  int ok = strcmp(rn, "File") == 0 && strcmp(mn, "expand_path") == 0;
  free(rn); free(mn);
  return ok ? sp_guard_operand(c->arguments->arguments.nodes[0], 1) : 0;
}

/* `__FILE__ == $0` and its spellings (either order, `$PROGRAM_NAME`, both
   sides under File.expand_path, `!=`): 1 when true, 2 when false, 0 when
   `node` is not the idiom. True in the entry script, false in a required
   file; `$0` itself stays the binary's argv[0]. */
static int sp_program_guard(pm_node_t *node) {
  if (PM_NODE_TYPE(node) != PM_CALL_NODE) return 0;
  pm_call_node_t *c = (pm_call_node_t *)node;
  if (!c->receiver || c->block || !c->arguments || c->arguments->arguments.size != 1) return 0;
  char *mn = cstr(c->name);
  int eq = strcmp(mn, "==") == 0 ? 1 : strcmp(mn, "!=") == 0 ? 0 : -1;
  free(mn);
  if (eq < 0) return 0;
  pm_node_t *rhs = c->arguments->arguments.nodes[0];
  int rk = sp_guard_operand(c->receiver, 0), ak = sp_guard_operand(rhs, 0);
  if (rk + ak != 3) return 0;
  if ((PM_NODE_TYPE(c->receiver) == PM_CALL_NODE) != (PM_NODE_TYPE(rhs) == PM_CALL_NODE)) return 0;
  int entry = sp_node_required_file(node) == NULL;
  return entry == eq ? 1 : 2;
}

/* ---- Sugar lowered on the tree ----
   Two shapes are flattened as the plain Ruby they stand for, which is what
   the analysis reads: an `&:name` block argument, and Forwardable's
   def_delegator(s) over literal Symbols. The plain Ruby is Prism nodes built
   here, each at the position of the source it stands for and owned by
   nothing in the tree: flatten writes them as it writes the parsed ones, and
   destroys them once written. They used to be text written over the source
   before Prism read it, by a lexer of their own, which misread a `&:name` in
   a regexp literal and the block argument of a command call nested in
   parentheses, and moved the column of everything after it on the line. */
static void *sp_made(pm_node_type_t type, size_t size, const pm_node_t *at) {
  pm_node_t *n = calloc(1, size);
  if (!n) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  n->type = type;
  n->location = at->location;
  return n;
}
static void sp_append(pm_node_list_t *list, pm_node_t *n) {
  size_t was = list->size;
  pm_node_list_append(list, n);
  if (list->size == was) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
}
/* The constant id of a name a made node carries. */
static pm_constant_id_t sp_made_id(const char *s, size_t len) {
  uint8_t *copy = malloc(len + 1);
  if (!copy) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(copy, s, len);
  pm_constant_id_t id = pm_constant_pool_insert_owned(&((pm_parser_t *)g_parser)->constant_pool, copy, len);
  if (!id) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  return id;
}
static pm_node_t *sp_made_lvar(const char *name, const pm_node_t *at) {
  pm_local_variable_read_node_t *v = sp_made(PM_LOCAL_VARIABLE_READ_NODE, sizeof *v, at);
  v->name = sp_made_id(name, strlen(name));
  return &v->base;
}
/* `recv.name` (a vcall when recv is NULL and `vcall`), with `arg` its one
   argument when there is one. */
static pm_node_t *sp_made_call(pm_node_t *recv, pm_constant_id_t name, pm_node_t *arg, int vcall,
                               const pm_node_t *at) {
  pm_call_node_t *c = sp_made(PM_CALL_NODE, sizeof *c, at);
  c->receiver = recv;
  c->name = name;
  if (vcall) c->base.flags |= PM_CALL_NODE_FLAGS_VARIABLE_CALL;
  if (!recv || PM_NODE_TYPE(recv) == PM_SELF_NODE) c->base.flags |= PM_CALL_NODE_FLAGS_IGNORE_VISIBILITY;
  if (arg) {
    c->arguments = sp_made(PM_ARGUMENTS_NODE, sizeof *c->arguments, at);
    sp_append(&c->arguments->arguments, arg);
  }
  return &c->base;
}
static pm_statements_node_t *sp_made_stmts(pm_node_t *n, const pm_node_t *at) {
  pm_statements_node_t *s = sp_made(PM_STATEMENTS_NODE, sizeof *s, at);
  sp_append(&s->body, n);
  return s;
}
/* The text of a plain Symbol literal (`:name`, `:@ivar`, `:<<`), a slice of
   the source; 0 for any other node (`:"name"`, `%s(name)`, a variable). */
static size_t sp_plain_sym(const pm_node_t *n, const char **s) {
  if (!n || PM_NODE_TYPE(n) != PM_SYMBOL_NODE) return 0;
  const pm_symbol_node_t *y = (const pm_symbol_node_t *)n;
  if (!y->opening_loc.start || y->opening_loc.end - y->opening_loc.start != 1 ||
      *y->opening_loc.start != ':' || !y->value_loc.start || y->closing_loc.start) return 0;
  *s = (const char *)y->value_loc.start;
  return (size_t)(y->value_loc.end - y->value_loc.start);
}
/* The text of a quoted Symbol with no interpolation or escape
   (`:'Faraday::Utils'`, `:"Utils"`), as the receiver of def_delegators names
   a constant path; 0 for any other node. */
static size_t sp_quoted_recv_sym(const pm_node_t *n, const char **s) {
  if (!n || PM_NODE_TYPE(n) != PM_SYMBOL_NODE) return 0;
  const pm_symbol_node_t *y = (const pm_symbol_node_t *)n;
  if (!y->opening_loc.start || y->opening_loc.end - y->opening_loc.start != 2 ||
      y->opening_loc.start[0] != ':' || (y->opening_loc.start[1] != '"' && y->opening_loc.start[1] != '\'') ||
      !y->closing_loc.start || !y->value_loc.start) return 0;
  for (const uint8_t *p = y->value_loc.start; p < y->value_loc.end; p++) if (*p == '\\' || *p == '#') return 0;
  *s = (const char *)y->value_loc.start;
  return (size_t)(y->value_loc.end - y->value_loc.start);
}
static int sp_word_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}
/* The text of a String literal with no interpolation or escape (`"@io"`,
   `'size'`), a slice of the source; 0 for any other node. Forwardable takes a
   String wherever it takes a Symbol: an accessor is source text either way,
   a method name the same name. */
static size_t sp_plain_str(const pm_node_t *n, const char **s) {
  if (!n || PM_NODE_TYPE(n) != PM_STRING_NODE) return 0;
  const pm_string_node_t *y = (const pm_string_node_t *)n;
  if (!y->opening_loc.start || y->opening_loc.end - y->opening_loc.start != 1 ||
      (*y->opening_loc.start != '"' && *y->opening_loc.start != '\'') ||
      !y->closing_loc.start || !y->content_loc.start) return 0;
  for (const uint8_t *p = y->content_loc.start; p < y->content_loc.end; p++) if (*p == '\\') return 0;
  *s = (const char *)y->content_loc.start;
  return (size_t)(y->content_loc.end - y->content_loc.start);
}
/* A method name given as a String: word characters, with a trailing ?, ! or
   = (the shapes a plain Symbol of a method name has besides operators). */
static int sp_str_method_name(const char *s, size_t n) {
  size_t e = n;
  if (e > 1 && (s[e - 1] == '?' || s[e - 1] == '!' || s[e - 1] == '=')) e--;
  if (e == 0 || (s[0] >= '0' && s[0] <= '9')) return 0;
  for (size_t i = 0; i < e; i++)
    if (!sp_word_char(s[i]) && (unsigned char)s[i] < 0x80) return 0;
  return 1;
}
/* The source text of a constant accessor (`def_delegators ColorParser, ...`,
   `Faraday::Utils`): Forwardable writes the Module's name where a Symbol
   accessor's text goes. A path from the top (`::X`) is not taken. */
static size_t sp_const_accessor(const pm_node_t *n, const char **s) {
  const pm_node_t *p = n;
  while (p && PM_NODE_TYPE(p) == PM_CONSTANT_PATH_NODE) p = ((const pm_constant_path_node_t *)p)->parent;
  if (!p || PM_NODE_TYPE(p) != PM_CONSTANT_READ_NODE) return 0;
  *s = (const char *)n->location.start;
  return (size_t)(n->location.end - n->location.start);
}

/* `&:name` as a block argument: the block `{ |_spx| _spx.name }` (marked
   sym_proc_block, which a user's own `{ |_spx| _spx.m }` is not), for a
   name of word characters (a byte past ASCII is one, `&:café`). Spinel
   lowers the native shape only for a few methods (inject / reduce /
   sort_by); every other block taker reads the block. A unary operator
   (`&:-@`, `&:+@`, `&:~`) is the same call, `-_spx` (#4300). NULL for any
   other block argument: a binary operator (`&:+`) is left to the
   arithmetic reduce/inject lowering, the rest to the analysis. */
static size_t sp_quoted_recv_sym(const pm_node_t *n, const char **s);
static pm_node_t *sp_sym_proc_block(pm_node_t *node) {
  if (PM_NODE_TYPE(node) != PM_BLOCK_ARGUMENT_NODE) return NULL;
  const char *s;
  /* the Symbol written in parentheses (`&(:to_s)`) or quoted (`&:"to_s"`)
     is the same block: left a BlockArgumentNode, a Symbol took no block */
  const pm_node_t *e = ((pm_block_argument_node_t *)node)->expression;
  while (e && PM_NODE_TYPE(e) == PM_PARENTHESES_NODE) {
    const pm_node_t *pb = ((const pm_parentheses_node_t *)e)->body;
    if (!pb || PM_NODE_TYPE(pb) != PM_STATEMENTS_NODE || ((const pm_statements_node_t *)pb)->body.size != 1) { e = NULL; break; }
    e = ((const pm_statements_node_t *)pb)->body.nodes[0];
  }
  size_t n = sp_plain_sym(e, &s);
  if (!n) n = sp_quoted_recv_sym(e, &s);
  int word = n > 0;
  for (size_t i = 0; i < n; i++)
    if (!sp_word_char(s[i]) && s[i] != '?' && s[i] != '!' && (unsigned char)s[i] < 0x80) word = 0;
  if (!word && !(n == 2 && (s[0] == '-' || s[0] == '+') && s[1] == '@') && !(n == 1 && s[0] == '~'))
    return NULL;
  pm_block_node_t *b = sp_made(PM_BLOCK_NODE, sizeof *b, node);
  pm_block_parameters_node_t *bp = sp_made(PM_BLOCK_PARAMETERS_NODE, sizeof *bp, node);
  pm_required_parameter_node_t *x = sp_made(PM_REQUIRED_PARAMETER_NODE, sizeof *x, node);
  x->name = sp_made_id("_spx", 4);
  if (!pm_constant_id_list_append(&b->locals, x->name)) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  bp->parameters = sp_made(PM_PARAMETERS_NODE, sizeof *bp->parameters, node);
  sp_append(&bp->parameters->requireds, &x->base);
  b->parameters = &bp->base;
  pm_node_t *call = sp_made_call(sp_made_lvar("_spx", node), sp_made_id(s, n), NULL, 0, node);
  b->body = &sp_made_stmts(call, node)->base;
  return &b->base;
}

/* The receiver `def_delegators :recv` names, as Ruby reads `recv.m`: an
   ivar (`:@io`), a constant, self, nil, true, false, or a method of the
   object (a vcall unless it ends in `?` or `!`), or a chain of method calls
   on one of those (`"@resolver.color"`, as pastel writes it: Forwardable
   pastes the accessor's text in front of the call). A new node per read;
   NULL for any other spelling. */
static pm_node_t *sp_fw_recv(const char *s, size_t n, const pm_node_t *at) {
  if (n == 0) return NULL;
  /* `recv.name`: the last call of a chain, on the receiver before it */
  size_t dot = 0;
  for (size_t k = 1; k + 1 < n; k++) if (s[k] == '.') dot = k;
  if (dot) {
    const char *m = s + dot + 1;
    size_t ml = n - dot - 1;
    size_t me = ml > 1 && (m[ml - 1] == '?' || m[ml - 1] == '!') ? ml - 1 : ml;
    if (m[0] >= '0' && m[0] <= '9') return NULL;
    for (size_t j = 0; j < me; j++) if (!sp_word_char(m[j])) return NULL;
    pm_node_t *recv = sp_fw_recv(s, dot, at);
    if (!recv) return NULL;
    return sp_made_call(recv, sp_made_id(m, ml), NULL, 0, at);
  }
  /* a constant path (`:'Faraday::Utils'`): the last segment a constant
     name, on the path before it */
  size_t last = 0;
  for (size_t k = 1; k + 1 < n; k++) if (s[k] == ':' && s[k + 1] == ':') last = k;
  if (last) {
    const char *t = s + last + 2;
    size_t tl = n - last - 2;
    if (tl == 0 || t[0] < 'A' || t[0] > 'Z') return NULL;
    for (size_t j = 0; j < tl; j++) if (!sp_word_char(t[j])) return NULL;
    pm_node_t *parent = sp_fw_recv(s, last, at);
    if (!parent) return NULL;
    if (PM_NODE_TYPE(parent) != PM_CONSTANT_READ_NODE && PM_NODE_TYPE(parent) != PM_CONSTANT_PATH_NODE) {
      pm_node_destroy((pm_parser_t *)g_parser, parent);
      return NULL;
    }
    pm_constant_path_node_t *cp = sp_made(PM_CONSTANT_PATH_NODE, sizeof *cp, at);
    cp->parent = parent;
    cp->name = sp_made_id(t, tl);
    return &cp->base;
  }
  size_t i = s[0] == '@' ? 1 : 0, e = n;
  if (i == 0 && n > 1 && (s[n - 1] == '?' || s[n - 1] == '!')) e = n - 1;
  if (i >= e || (s[i] >= '0' && s[i] <= '9')) return NULL;
  for (size_t k = i; k < e; k++) if (!sp_word_char(s[k])) return NULL;
  pm_node_type_t lit = n == 4 && !memcmp(s, "self", 4) ? PM_SELF_NODE : n == 3 && !memcmp(s, "nil", 3) ? PM_NIL_NODE
                     : n == 4 && !memcmp(s, "true", 4) ? PM_TRUE_NODE : n == 5 && !memcmp(s, "false", 5) ? PM_FALSE_NODE : 0;
  if (lit) return sp_made(lit, sizeof(pm_node_t), at);
  if (i == 1) {
    pm_instance_variable_read_node_t *v = sp_made(PM_INSTANCE_VARIABLE_READ_NODE, sizeof *v, at);
    v->name = sp_made_id(s, n);
    return &v->base;
  }
  if (s[0] >= 'A' && s[0] <= 'Z' && e == n) {
    pm_constant_read_node_t *k = sp_made(PM_CONSTANT_READ_NODE, sizeof *k, at);
    k->name = sp_made_id(s, n);
    return &k->base;
  }
  return sp_made_call(NULL, sp_made_id(s, n), NULL, e == n, at);
}

/* One forwarding def, `als` delegating to `meth` of the receiver:
   `def als(*_fw_a) = _fw_a.length == 0 ? R.meth : R.meth(*_fw_a)`; for an
   `each` / `each_*` target the caller's block too, as `&_fw_b` where one
   was given. The arity
   split keeps a zero-argument delegation off the splat path (a builtin
   target's optional-argument arm would otherwise see the empty rest array
   as an argument). A setter alias (`:timer_a=`) is
   `def als(_fw_v); R.target = _fw_v; end`, the target written as the
   assignment the delegate would make, whether or not `meth` is itself a
   setter (#4518). `def self.als` under def_single_delegator(s). */
static pm_node_t *sp_fw_def(const char *r, size_t rl, const char *meth, size_t ml, const char *als, size_t al,
                            int single, const pm_node_t *at) {
  pm_def_node_t *d = sp_made(PM_DEF_NODE, sizeof *d, at);
  d->name = sp_made_id(als, al);
  if (single) d->receiver = sp_made(PM_SELF_NODE, sizeof(pm_self_node_t), at);
  d->parameters = sp_made(PM_PARAMETERS_NODE, sizeof *d->parameters, at);
  pm_node_t *body;
  if (al > 1 && als[al - 1] == '=' && sp_word_char(als[al - 2])) {
    pm_required_parameter_node_t *v = sp_made(PM_REQUIRED_PARAMETER_NODE, sizeof *v, at);
    v->name = sp_made_id("_fw_v", 5);
    sp_append(&d->parameters->requireds, &v->base);
    size_t tl = ml > 1 && meth[ml - 1] == '=' ? ml - 1 : ml;
    char *tn = malloc(tl + 1);
    if (!tn) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    memcpy(tn, meth, tl);
    tn[tl] = '=';
    body = sp_made_call(sp_fw_recv(r, rl, at), sp_made_id(tn, tl + 1), sp_made_lvar("_fw_v", at), 0, at);
    body->flags |= PM_CALL_NODE_FLAGS_ATTRIBUTE_WRITE;   /* written as the assignment */
    free(tn);
  }
  else {
    pm_rest_parameter_node_t *rest = sp_made(PM_REST_PARAMETER_NODE, sizeof *rest, at);
    rest->name = sp_made_id("_fw_a", 5);
    d->parameters->rest = &rest->base;
    /* The caller's block goes on to an each target: `def_delegators :@a,
       :each` with `include Enumerable` reaches each with Enumerable's
       block, and without it every Enumerable method answered empty. Only
       an each, whose target is an iterator too: another name may be a
       builtin that takes no block, which has no arm for one. */
    int blk = (ml == 4 && !memcmp(meth, "each", 4)) || (ml > 5 && !memcmp(meth, "each_", 5));
    if (blk) {
      pm_block_parameter_node_t *bp = sp_made(PM_BLOCK_PARAMETER_NODE, sizeof *bp, at);
      bp->name = sp_made_id("_fw_b", 5);
      d->parameters->block = bp;
    }
    /* `block_given? ? <split with &_fw_b> : <split without>`: a call with
       no block hands the target none, and each answers its Enumerator */
    pm_node_t *arm[2] = { NULL, NULL };
    for (int wb = 0; wb < 1 + blk; wb++) {
      pm_integer_node_t *zero = sp_made(PM_INTEGER_NODE, sizeof *zero, at);
      pm_node_t *len = sp_made_call(sp_made_lvar("_fw_a", at), sp_made_id("length", 6), NULL, 0, at);
      pm_if_node_t *f = sp_made(PM_IF_NODE, sizeof *f, at);
      f->predicate = sp_made_call(len, sp_made_id("==", 2), &zero->base, 0, at);
      pm_node_t *c0 = sp_made_call(sp_fw_recv(r, rl, at), sp_made_id(meth, ml), NULL, 0, at);
      f->statements = sp_made_stmts(c0, at);
      pm_splat_node_t *all = sp_made(PM_SPLAT_NODE, sizeof *all, at);
      all->expression = sp_made_lvar("_fw_a", at);
      pm_else_node_t *e = sp_made(PM_ELSE_NODE, sizeof *e, at);
      pm_node_t *c1 = sp_made_call(sp_fw_recv(r, rl, at), sp_made_id(meth, ml), &all->base, 0, at);
      e->statements = sp_made_stmts(c1, at);
      f->subsequent = &e->base;
      if (wb) {
        pm_node_t *cs[2] = { c0, c1 };
        for (int k = 0; k < 2; k++) {
          pm_block_argument_node_t *ba = sp_made(PM_BLOCK_ARGUMENT_NODE, sizeof *ba, at);
          ba->expression = sp_made_lvar("_fw_b", at);
          ((pm_call_node_t *)cs[k])->block = &ba->base;
        }
      }
      arm[wb] = &f->base;
    }
    if (blk) {
      pm_if_node_t *g = sp_made(PM_IF_NODE, sizeof *g, at);
      g->predicate = sp_made_call(NULL, sp_made_id("block_given?", 12), NULL, 1, at);
      g->statements = sp_made_stmts(arm[1], at);
      pm_else_node_t *ge = sp_made(PM_ELSE_NODE, sizeof *ge, at);
      ge->statements = sp_made_stmts(arm[0], at);
      g->subsequent = &ge->base;
      body = &g->base;
    }
    else body = arm[0];
  }
  d->body = &sp_made_stmts(body, at)->base;
  return &d->base;
}

static int sp_const_symbol_list(const char *src, size_t len, size_t before, const char *name,
                                char (**out)[160], int *nout);
/* Forwardable's `def_delegator :recv, :meth[, :alias]` and
   `def_delegators :recv, :m1, :m2, ...` (and def_instance_delegator(s),
   their aliases, and SingleForwardable's def_single_delegator(s), which
   delegate at the class level) over literal Symbols or Strings, or the names a
   constant holds (`*NAMES`, sp_const_symbol_list, as Rack::Lint writes it);
   the accessor may also be a constant (`ColorParser`, sp_const_accessor),
   as a statement on lines of its own: the forwarding defs it stands for,
   appended to `out`. 0 for any other statement; a def_delegators left as a
   call is refused where it is written (check_unrewritten_delegators). */
static int sp_lower_delegators(pm_node_t *stmt, pm_node_list_t *out) {
  if (PM_NODE_TYPE(stmt) != PM_CALL_NODE) return 0;
  pm_call_node_t *c = (pm_call_node_t *)stmt;
  if (c->receiver || c->block || !c->arguments || c->arguments->arguments.size < 2) return 0;
  const pm_constant_t *k = &g_parser->constant_pool.constants[c->name - 1];
  const char *nm = (const char *)k->start;
  size_t bl = k->length > 0 && nm[k->length - 1] == 's' ? k->length - 1 : k->length;
  int plural = bl < k->length, single = bl == 20 && !memcmp(nm, "def_single_delegator", 20);
  if (!single && !(bl == 13 && !memcmp(nm, "def_delegator", 13)) &&
      !(bl == 22 && !memcmp(nm, "def_instance_delegator", 22))) return 0;
  const uint8_t *src = g_parser->start, *p = stmt->location.start, *q = stmt->location.end;
  while (p > src && (p[-1] == ' ' || p[-1] == '\t')) p--;
  if (p > src && p[-1] != '\n' && p[-1] != ';') return 0;
  while (q < g_parser->end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
  if (q < g_parser->end && *q != '\n' && *q != '#') return 0;
  pm_node_list_t *args = &c->arguments->arguments;
  const char *r;
  size_t rl = sp_plain_sym(args->nodes[0], &r);
  if (!rl) rl = sp_quoted_recv_sym(args->nodes[0], &r);
  if (!rl) rl = sp_plain_str(args->nodes[0], &r);
  if (!rl) rl = sp_const_accessor(args->nodes[0], &r);
  pm_node_t *probe = rl ? sp_fw_recv(r, rl, stmt) : NULL;
  if (!probe) return 0;
  pm_node_destroy((pm_parser_t *)g_parser, probe);
  /* the names, each a copy */
  size_t n = 0, cap = args->size + 8;
  char **names = malloc(cap * sizeof *names);
  if (!names) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  int ok = 1;
  for (size_t i = 1; i < args->size && ok; i++) {
    pm_node_t *a = args->nodes[i];
    const char *s;
    size_t sl = sp_plain_sym(a, &s);
    char (*cs)[160] = NULL;
    int ncs = 0;
    if (sl && s[0] != '@' && s[0] != '$') ncs = 1;
    else if (!sl && (sl = sp_plain_str(a, &s)) > 0 && sp_str_method_name(s, sl)) ncs = 1;
    else if (PM_NODE_TYPE(a) == PM_SPLAT_NODE && ((pm_splat_node_t *)a)->expression &&
             PM_NODE_TYPE(((pm_splat_node_t *)a)->expression) == PM_CONSTANT_READ_NODE) {
      char *cn = cstr(((pm_constant_read_node_t *)((pm_splat_node_t *)a)->expression)->name);
      if (!sp_const_symbol_list((const char *)src, (size_t)(g_parser->end - src),
                                (size_t)(stmt->location.start - src), cn, &cs, &ncs)) ok = 0;
      free(cn);
    }
    else ok = 0;
    for (int j = 0; j < ncs && ok; j++) {
      if (n == cap) {
        cap *= 2;
        char **nn = realloc(names, cap * sizeof *names);
        if (!nn) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
        names = nn;
      }
      size_t l = cs ? strlen(cs[j]) : sl;
      char *nm1 = malloc(l + 1);
      if (!nm1) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      memcpy(nm1, cs ? cs[j] : s, l);
      nm1[l] = '\0';
      names[n++] = nm1;
    }
    free(cs);
  }
  if (ok && (n == 0 || (!plural && n > 2))) ok = 0;
  if (ok && !plural)
    sp_append(out, sp_fw_def(r, rl, names[0], strlen(names[0]), names[n - 1], strlen(names[n - 1]),
                             single, stmt));
  for (size_t i = 0; ok && plural && i < n; i++)
    sp_append(out, sp_fw_def(r, rl, names[i], strlen(names[i]), names[i], strlen(names[i]), single, stmt));
  for (size_t i = 0; i < n; i++) free(names[i]);
  free(names);
  return ok;
}

/* `body` with each def_delegators statement written as its defs, into `out`
   (the defs also into `made`, destroyed once flattened); 0 when no
   statement is one, and `out` is left empty. */
static int sp_lower_body(const pm_node_list_t *body, pm_node_list_t *out, pm_node_list_t *made) {
  int any = 0;
  for (size_t i = 0; i < body->size; i++) {
    size_t from = made->size;
    if (sp_lower_delegators(body->nodes[i], made)) {
      for (size_t j = 0; !any && j < i; j++) sp_append(out, body->nodes[j]);
      any = 1;
      for (size_t j = from; j < made->size; j++) sp_append(out, made->nodes[j]);
    }
    else if (any) sp_append(out, body->nodes[i]);
  }
  return any;
}

/* ---- Main flattening ---- */
/* The builtin node the names invented from the node being flattened count
   from (`node_bi`, comp_node_ord): its enclosing def, or, outside every def,
   its top-level statement. -1 outside builtins/. */
/* `def m(a, ...)` in a class that inherits Hash or Array, whose `...`
   only a bare `super` uses (faraday's Headers#fetch): the method is
   `def m(a, *__fwd_r, &__fwd_b)`, which the super into the builtin passes
   on whole. The `...` model binds one slot per argument some caller of the
   name passes, so `fetch(k)` beside a `fetch(k, default)` handed the
   builtin a nil default. A builtin method of these takes no keywords, and
   initialize (Hash.new's capacity:) keeps its own path. */
static bool sp_fwd_uses(const pm_node_t *node, void *data) {
  int *u = data;
  if (PM_NODE_TYPE(node) == PM_FORWARDING_SUPER_NODE) u[0]++;
  else if (PM_NODE_TYPE(node) == PM_FORWARDING_ARGUMENTS_NODE) u[1]++;
  else if (PM_NODE_TYPE(node) == PM_DEF_NODE) u[2]++;
  return true;
}
static int sp_builtin_coll_super(const pm_node_t *sup) {
  if (!sup) return 0;
  pm_constant_id_t nm;
  if (PM_NODE_TYPE(sup) == PM_CONSTANT_READ_NODE) nm = ((const pm_constant_read_node_t *)sup)->name;
  else if (PM_NODE_TYPE(sup) == PM_CONSTANT_PATH_NODE && !((const pm_constant_path_node_t *)sup)->parent)
    nm = ((const pm_constant_path_node_t *)sup)->name;
  else return 0;
  const pm_constant_t *k = &g_parser->constant_pool.constants[nm - 1];
  return (k->length == 4 && !memcmp(k->start, "Hash", 4)) || (k->length == 5 && !memcmp(k->start, "Array", 5));
}
static void sp_fwd_super_to_rest(pm_class_node_t *cls, const pm_node_t *sup) {
  if (!sp_builtin_coll_super(sup) || !cls->body || PM_NODE_TYPE(cls->body) != PM_STATEMENTS_NODE) return;
  pm_node_list_t *body = &((pm_statements_node_t *)cls->body)->body;
  for (size_t i = 0; i < body->size; i++) {
    if (PM_NODE_TYPE(body->nodes[i]) != PM_DEF_NODE) continue;
    pm_def_node_t *d = (pm_def_node_t *)body->nodes[i];
    pm_parameters_node_t *ps = d->parameters;
    if (d->receiver || !ps || !ps->keyword_rest || PM_NODE_TYPE(ps->keyword_rest) != PM_FORWARDING_PARAMETER_NODE) continue;
    if (ps->rest || ps->block || ps->keywords.size || ps->posts.size) continue;
    const pm_constant_t *k = &g_parser->constant_pool.constants[d->name - 1];
    if (k->length == 10 && !memcmp(k->start, "initialize", 10)) continue;
    int u[3] = { 0, 0, 0 };
    if (d->body) pm_visit_node(d->body, sp_fwd_uses, u);
    if (!u[0] || u[1] || u[2]) continue;
    pm_node_destroy((pm_parser_t *)g_parser, ps->keyword_rest);
    ps->keyword_rest = NULL;
    pm_rest_parameter_node_t *r = sp_made(PM_REST_PARAMETER_NODE, sizeof *r, &d->base);
    r->name = sp_made_id("__fwd_r", 7);
    ps->rest = &r->base;
    pm_block_parameter_node_t *b = sp_made(PM_BLOCK_PARAMETER_NODE, sizeof *b, &d->base);
    b->name = sp_made_id("__fwd_b", 7);
    ps->block = b;
  }
}

static int g_bi_base = -1;
static int flatten_node(pm_node_t *node);
static int flatten(pm_node_t *node) {
  int saved = g_bi_base;
  /* `&:name` is flattened as the block it stands for */
  pm_node_t *made = node ? sp_sym_proc_block(node) : NULL;
  if (made) node = made;
  /* the node's inline RBS facts, under the id flatten_node gives it next */
  if (node) sp_inline_rbs_emit(node, node_counter);
  if (node && PM_NODE_TYPE(node) == PM_CLASS_NODE)
    sp_fwd_super_to_rest((pm_class_node_t *)node, ((pm_class_node_t *)node)->superclass);
  int id = flatten_node(node);
  if (made) pm_node_destroy((pm_parser_t *)g_parser, made);
  g_bi_base = saved;
  return id;
}

/* A StatementsNode's body, each statement under the statement cursor; a
   def_delegators statement is flattened as the defs it stands for. */
static void flatten_statements(int id, pm_node_list_t *list) {
  pm_node_t **next = g_stmt_next, **end = g_stmt_end;
  pm_node_list_t low = { 0 }, made = { 0 };
  pm_node_list_t *body = sp_lower_body(list, &low, &made) ? &low : list;
  g_stmt_next = body->nodes; g_stmt_end = g_stmt_next + body->size;
  emit_node_array(id, "body", body);
  g_stmt_next = next; g_stmt_end = end;
  for (size_t i = 0; i < made.size; i++) pm_node_destroy((pm_parser_t *)g_parser, made.nodes[i]);
  pm_node_list_free(&made);
  pm_node_list_free(&low);
}

/* The parameter node kinds: the name, an optional default, and Prism's flags,
   whose PM_PARAMETER_FLAGS_REPEATED_PARAMETER marks a name an earlier
   parameter of the list already bound (`def m(_, _)`; nt_param_repeated). */
static void flatten_parameter(int id, pm_node_t *node) {
  const char *type = NULL;
  pm_constant_id_t name = 0;
  pm_node_t *value = NULL;
  switch (PM_NODE_TYPE(node)) {
  case PM_REQUIRED_PARAMETER_NODE:
    type = "RequiredParameterNode"; name = ((pm_required_parameter_node_t *)node)->name; break;
  case PM_OPTIONAL_PARAMETER_NODE:
    type = "OptionalParameterNode"; name = ((pm_optional_parameter_node_t *)node)->name;
    value = ((pm_optional_parameter_node_t *)node)->value; break;
  case PM_REST_PARAMETER_NODE:
    type = "RestParameterNode"; name = ((pm_rest_parameter_node_t *)node)->name; break;
  case PM_BLOCK_PARAMETER_NODE:
    type = "BlockParameterNode"; name = ((pm_block_parameter_node_t *)node)->name; break;
  case PM_BLOCK_LOCAL_VARIABLE_NODE:
    type = "BlockLocalVariableNode"; name = ((pm_block_local_variable_node_t *)node)->name; break;
  case PM_KEYWORD_REST_PARAMETER_NODE:
    type = "KeywordRestParameterNode"; name = ((pm_keyword_rest_parameter_node_t *)node)->name; break;
  case PM_REQUIRED_KEYWORD_PARAMETER_NODE:
    type = "RequiredKeywordParameterNode"; name = ((pm_required_keyword_parameter_node_t *)node)->name; break;
  default:
    type = "OptionalKeywordParameterNode"; name = ((pm_optional_keyword_parameter_node_t *)node)->name;
    value = ((pm_optional_keyword_parameter_node_t *)node)->value; break;
  }
  nt_put_type(out_nt, id, type);
  emit_int(id, "flags", node->flags);
  if (name) { char *n = cstr(name); emit_str(id, "name", n); free(n); }
  if (value) emit_ref(id, "value", value);
}

/* The Prism flags of a CallNode that the passes read by name, each written
   only when Prism set it: `recv&.name` (nt_safe_nav); `recv.name = v`,
   `recv.name=(v)` and `recv[i] = v`, which `recv.send(:name=, v)` is not
   (nt_attr_write); and a bare identifier (no receiver/parens/args), a
   variable-or-method read whose unresolved form is CRuby's NameError, not
   NoMethodError (vcall). Private calls use Prism's receiverless or literal
   self permission (nt_ignore_visibility). */
static void emit_call_flags(int id, const pm_node_t *node) {
  if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_IGNORE_VISIBILITY)) emit_int(id, "ignore_visibility", 1);
  if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) emit_int(id, "safe_nav", 1);
  if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_ATTRIBUTE_WRITE)) emit_int(id, "attr_write", 1);
  if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_VARIABLE_CALL)) emit_int(id, "vcall", 1);
}
static void emit_index_write_fields(int id, const pm_node_t *node, pm_node_t *receiver,
                                    pm_arguments_node_t *arguments, pm_node_t *value) {
  emit_ref(id, "receiver", receiver);
  emit_ref(id, "arguments", (pm_node_t *)arguments);
  emit_ref(id, "value", value);
  emit_call_flags(id, node);
}
static int flatten_node(pm_node_t *node) {
  if (!node) return -1;

  int id = node_counter++;
  pm_node_type_t t = PM_NODE_TYPE(node);
  const uint8_t *owner = g_owner;
  /* a node spliced from builtins/ is stamped with the builtin node it counts
     from (+1); the program node and its statements are the program's even
     when the source begins with a splice */
  int in_bi = t != PM_PROGRAM_NODE && !(t == PM_STATEMENTS_NODE && !owner) &&
              sp_in_builtin(node->location.start);
  if (in_bi) {
    if (g_bi_base < 0 || t == PM_DEF_NODE) g_bi_base = id;
    emit_int(id, "node_bi", g_bi_base + 1);
  }
  if (g_stmt_next < g_stmt_end && node == *g_stmt_next) {
    int32_t bl = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);
    int32_t ol = owner ? pm_newline_list_line(&g_parser->newline_list, owner, g_parser->start_line) : 0;
    g_stmt_next++;
    if (bl >= 1 && bl <= sp_line_map_n && sp_line_pop[bl] > 0 && sp_line_pop[bl] != sp_line_pop[ol])
      emit_int(id, "req_pop", sp_line_pop[bl]);
  }
  if (t != PM_STATEMENTS_NODE) g_owner = t == PM_PROGRAM_NODE ? NULL : node->location.start;

  /* Debug builds only: stamp every node with its source line -- and, when a
     multi-file map was built, its original file -- so codegen can emit
     `#line N "file"` directives. Written to dedicated `node_line` /
     `node_file` fields (NOT the overloaded `value`/`start_line` slot). The
     raw value is the line in the concatenated buffer; the map translates it
     back to the original file and line. */
  if (g_emit_line) {
    pm_line_column_t lc = pm_newline_list_line_column(&g_parser->newline_list,
                                                      node->location.start,
                                                      g_parser->start_line);
    int32_t bl = lc.line;
    int orig = bl;
    int fid = 0;
    if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n && sp_line_orig[bl] > 0) {
      orig = sp_line_orig[bl];
      fid = sp_line_file[bl];
      if (sp_disp_line[bl] > 0) { orig = sp_disp_line[bl]; fid = sp_disp_file[bl]; }
    }
    emit_int(id, "node_line", (long long)orig);
    emit_int(id, "node_file", (long long)fid);
    /* Column is concatenation-stable (require splicing is line-based), so the
       buffer column equals the original-file column. 0-based, as Prism gives. */
    emit_int(id, "node_col", (long long)lc.column);
    if (g_emit_end) {
      /* Prism's end is exclusive; the line map is line-based, so the end
         line goes through it the way the start line does */
      pm_line_column_t le = pm_newline_list_line_column(&g_parser->newline_list,
                                                        node->location.end,
                                                        g_parser->start_line);
      int32_t el = le.line;
      int eorig = el;
      if (sp_line_map_n > 0 && el >= 1 && el <= sp_line_map_n && sp_line_orig[el] > 0) {
        eorig = sp_line_orig[el];
        if (sp_disp_line[el] > 0) eorig = sp_disp_line[el];
      }
      emit_int(id, "node_end_line", (long long)eorig);
      emit_int(id, "node_end_col", (long long)le.column);
    }
  }

#define N(type_name) nt_put_type(out_nt, id, type_name)
#define S(field, val) emit_str(id, field, val)
#define I(field, val) emit_int(id, field, val)
#define R(field, child) emit_ref(id, field, (pm_node_t *)(child))
#define A(field, list) emit_node_array(id, field, list)
#define NAME(field, cid) do { char *_n = cstr(cid); emit_str(id, field, _n); free(_n); } while(0)

  switch (t) {
  case PM_PROGRAM_NODE: {
    pm_program_node_t *n = (pm_program_node_t *)node;
    N("ProgramNode");
    R("statements", n->statements);
    break;
  }
  case PM_STATEMENTS_NODE:
    N("StatementsNode");
    flatten_statements(id, &((pm_statements_node_t *)node)->body);
    break;
  case PM_CLASS_NODE: {
    pm_class_node_t *n = (pm_class_node_t *)node;
    N("ClassNode");
    R("constant_path", n->constant_path);
    /* `class A < (Base)`: parentheses around a single expression carry no
       meaning, but every pass reads the superclass by its node kind, and a
       ParenthesesNode read as no superclass at all (Object). */
    pm_node_t *sup = n->superclass;
    while (sup && PM_NODE_TYPE(sup) == PM_PARENTHESES_NODE) {
      pm_node_t *pb = ((pm_parentheses_node_t *)sup)->body;
      if (!pb || PM_NODE_TYPE(pb) != PM_STATEMENTS_NODE ||
          ((pm_statements_node_t *)pb)->body.size != 1) break;
      sup = ((pm_statements_node_t *)pb)->body.nodes[0];
    }
    R("superclass", sup);
    R("body", n->body);
    break;
  }
  case PM_MODULE_NODE: {
    pm_module_node_t *n = (pm_module_node_t *)node;
    N("ModuleNode");
    R("constant_path", n->constant_path);
    R("body", n->body);
    break;
  }
  case PM_SINGLETON_CLASS_NODE: {
    /* `class << self; ...; end` -- the singleton class block. We
       only support `expression == SelfNode` today (i.e. the
       enclosing class/module's singleton). The body is flattened
       up one level so codegen sees `attr_accessor :x` / `def foo`
       inside the parent ClassNode/ModuleNode body, and the
       SingletonClassNode marker survives so dispatch can route
       methods/accessors to the class-method path. */
    pm_singleton_class_node_t *n = (pm_singleton_class_node_t *)node;
    N("SingletonClassNode");
    R("expression", n->expression);
    R("body", n->body);
    break;
  }
  case PM_DEF_NODE: {
    pm_def_node_t *n = (pm_def_node_t *)node;
    N("DefNode");
    NAME("name", n->name);
    R("parameters", n->parameters);
    R("body", n->body);
    R("receiver", n->receiver);
    break;
  }
  case PM_CALL_NODE: {
    pm_call_node_t *n = (pm_call_node_t *)node;
    { int pg = sp_program_guard(node);
      if (pg) {
        if (pg == 1) N("TrueNode"); else N("FalseNode");
        I("program_guard", 1);
        break;
      } }
    N("CallNode");
    NAME("name", n->name);
    /* a bare `warn`: the line and file it was written in, which `uplevel: 0`
       prints ahead of the message -- stamped as __LINE__ and __FILE__ are,
       since node positions exist only under the line map. `Kernel.warn` is
       the same call. */
    if (!n->receiver || PM_NODE_TYPE(n->receiver) == PM_CONSTANT_READ_NODE) {
      char *wn = cstr(n->name);
      if (strcmp(wn, "warn") == 0) {
        int32_t wl = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);
        if (sp_line_map_n > 0 && wl >= 1 && wl <= sp_line_map_n && sp_line_orig[wl] > 0) wl = sp_line_orig[wl];
        I("warn_line", (long long)wl);
        const char *rf = sp_node_required_file(node);
        if (rf) {
          char *ap = sp_required_file_path(rf);
          emit_str(id, "warn_path", ap);
          free(ap);
        }
        else emit_str(id, "warn_path", g_source_file);
      }
      free(wn);
    }
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("block", n->block);
    emit_call_flags(id, node);
    /* `__dir__` in a required file is that file's directory: carry the file
       for the compile-time folds, which otherwise see only the entry script */
    if (!n->receiver && !n->arguments) {
      char *cn = cstr(n->name);
      const char *rf = strcmp(cn, "__dir__") == 0 ? sp_node_required_file(node) : NULL;
      if (rf) {
        char *ap = sp_required_file_path(rf);
        emit_str(id, "src_file", ap);
        free(ap);
      }
      free(cn);
    }
    break;
  }
  case PM_CONSTANT_WRITE_NODE: {
    pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
    N("ConstantWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_PATH_WRITE_NODE: {
    pm_constant_path_write_node_t *n = (pm_constant_path_write_node_t *)node;
    N("ConstantPathWriteNode");
    R("value", n->value);
    R("target", n->target);
    break;
  }
  case PM_CONSTANT_OPERATOR_WRITE_NODE: {
    pm_constant_operator_write_node_t *n = (pm_constant_operator_write_node_t *)node;
    N("ConstantOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_OR_WRITE_NODE: {
    pm_constant_or_write_node_t *n = (pm_constant_or_write_node_t *)node;
    N("ConstantOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_AND_WRITE_NODE: {
    pm_constant_and_write_node_t *n = (pm_constant_and_write_node_t *)node;
    N("ConstantAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {
    pm_constant_path_operator_write_node_t *n = (pm_constant_path_operator_write_node_t *)node;
    N("ConstantPathOperatorWriteNode");
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_PATH_OR_WRITE_NODE: {
    pm_constant_path_or_write_node_t *n = (pm_constant_path_or_write_node_t *)node;
    N("ConstantPathOrWriteNode");
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_PATH_AND_WRITE_NODE: {
    pm_constant_path_and_write_node_t *n = (pm_constant_path_and_write_node_t *)node;
    N("ConstantPathAndWriteNode");
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_READ_NODE: {
    pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
    N("ConstantReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_CONSTANT_PATH_NODE: {
    pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
    N("ConstantPathNode");
    R("parent", n->parent);
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
    N("LocalVariableWriteNode");
    NAME("name", n->name);
    I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_READ_NODE: {
    pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
    N("LocalVariableReadNode");
    NAME("name", n->name);
    I("depth", n->depth);
    break;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
    N("LocalVariableOperatorWriteNode");
    NAME("name", n->name);
    I("depth", n->depth);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
    N("LocalVariableOrWriteNode");
    NAME("name", n->name);
    I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
    N("LocalVariableAndWriteNode");
    NAME("name", n->name);
    I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)node;
    N("LocalVariableTargetNode");
    NAME("name", n->name);
    I("depth", n->depth);
    break;
  }
  case PM_INSTANCE_VARIABLE_WRITE_NODE: {
    pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
    N("InstanceVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_READ_NODE: {
    pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
    N("InstanceVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_TARGET_NODE: {
    pm_instance_variable_target_node_t *n = (pm_instance_variable_target_node_t *)node;
    N("InstanceVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CALL_TARGET_NODE: {
    pm_call_target_node_t *n = (pm_call_target_node_t *)node;
    N("CallTargetNode");
    NAME("name", n->name);
    R("receiver", n->receiver);
    emit_call_flags(id, node);
    break;
  }
  case PM_CONSTANT_TARGET_NODE: {
    pm_constant_target_node_t *n = (pm_constant_target_node_t *)node;
    N("ConstantTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CLASS_VARIABLE_TARGET_NODE: {
    pm_class_variable_target_node_t *n = (pm_class_variable_target_node_t *)node;
    N("ClassVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CONSTANT_PATH_TARGET_NODE: {
    pm_constant_path_target_node_t *n = (pm_constant_path_target_node_t *)node;
    N("ConstantPathTargetNode");
    R("parent", n->parent);
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {
    pm_instance_variable_and_write_node_t *n = (pm_instance_variable_and_write_node_t *)node;
    N("InstanceVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
    pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
    N("InstanceVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_instance_variable_operator_write_node_t *n = (pm_instance_variable_operator_write_node_t *)node;
    N("InstanceVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_WRITE_NODE: {
    pm_class_variable_write_node_t *n = (pm_class_variable_write_node_t *)node;
    N("ClassVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_READ_NODE: {
    pm_class_variable_read_node_t *n = (pm_class_variable_read_node_t *)node;
    N("ClassVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_class_variable_operator_write_node_t *n = (pm_class_variable_operator_write_node_t *)node;
    N("ClassVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_OR_WRITE_NODE: {
    pm_class_variable_or_write_node_t *n = (pm_class_variable_or_write_node_t *)node;
    N("ClassVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_AND_WRITE_NODE: {
    pm_class_variable_and_write_node_t *n = (pm_class_variable_and_write_node_t *)node;
    N("ClassVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INDEX_OPERATOR_WRITE_NODE: {
    pm_index_operator_write_node_t *n = (pm_index_operator_write_node_t *)node;
    N("IndexOperatorWriteNode");
    NAME("binary_operator", n->binary_operator);
    emit_index_write_fields(id, node, n->receiver, n->arguments, n->value);
    break;
  }
  case PM_INDEX_AND_WRITE_NODE: {
    pm_index_and_write_node_t *n = (pm_index_and_write_node_t *)node;
    N("IndexAndWriteNode");
    emit_index_write_fields(id, node, n->receiver, n->arguments, n->value);
    break;
  }
  case PM_INDEX_OR_WRITE_NODE: {
    pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)node;
    N("IndexOrWriteNode");
    emit_index_write_fields(id, node, n->receiver, n->arguments, n->value);
    break;
  }
  case PM_INDEX_TARGET_NODE: {
    pm_index_target_node_t *n = (pm_index_target_node_t *)node;
    N("IndexTargetNode");
    /* CRuby refuses private []= on masgn and for targets, even on self. */
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    break;
  }
  case PM_CALL_OPERATOR_WRITE_NODE: {
    pm_call_operator_write_node_t *n = (pm_call_operator_write_node_t *)node;
    N("CallOperatorWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    emit_call_flags(id, node);
    break;
  }
  case PM_CALL_AND_WRITE_NODE: {
    pm_call_and_write_node_t *n = (pm_call_and_write_node_t *)node;
    N("CallAndWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    emit_call_flags(id, node);
    break;
  }
  case PM_CALL_OR_WRITE_NODE: {
    pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)node;
    N("CallOrWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    emit_call_flags(id, node);
    break;
  }
  case PM_GLOBAL_VARIABLE_WRITE_NODE: {
    pm_global_variable_write_node_t *n = (pm_global_variable_write_node_t *)node;
    N("GlobalVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_READ_NODE: {
    pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
    N("GlobalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_global_variable_operator_write_node_t *n = (pm_global_variable_operator_write_node_t *)node;
    N("GlobalVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {
    pm_global_variable_or_write_node_t *n = (pm_global_variable_or_write_node_t *)node;
    N("GlobalVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: {
    pm_global_variable_and_write_node_t *n = (pm_global_variable_and_write_node_t *)node;
    N("GlobalVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_TARGET_NODE: {
    pm_global_variable_target_node_t *n = (pm_global_variable_target_node_t *)node;
    N("GlobalVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_NO_KEYWORDS_PARAMETER_NODE: {
    /* `def f(**nil)` -- explicit kwarg rejection. Spinel's keyword-arg
       handling is already conservative (only known keys accepted),
       so the explicit "no keywords" marker is effectively a no-op
       at the codegen level. We emit the node so a ParametersNode
       carrying it doesn't leave a NULL keyword slot, but the
       compile-time effect is nothing. */
    N("NoKeywordsParameterNode");
    break;
  }
  case PM_INTEGER_NODE: {
    pm_integer_node_t *n = (pm_integer_node_t *)node;
    N("IntegerNode");
    I("value", pm_int_value(&n->value));
    /* A literal wider than int64 (`100000000000000000000`) saturates `value`;
       keep the exact decimal text so codegen can build an arbitrary-precision
       bigint (`sp_bigint_new_str`). CRuby treats it as a Bignum in every mode. */
    if (pm_int_overflows(&n->value)) S("bigval", pm_int_text(&n->value));
    break;
  }
  case PM_FLOAT_NODE: {
    pm_float_node_t *n = (pm_float_node_t *)node;
    N("FloatNode");
    emit_float(id, n->value);
    break;
  }
  case PM_IMAGINARY_NODE: {
    /* `2i` -- imaginary numeric. Issue #840. Wraps an inner
       IntegerNode or FloatNode in `numeric`. Codegen surfaces this
       as a sp_Complex literal {re=0, im=numeric}. */
    pm_imaginary_node_t *n = (pm_imaginary_node_t *)node;
    N("ImaginaryNode");
    R("numeric", n->numeric);
    break;
  }
  case PM_RATIONAL_NODE: {
    /* `1/2r` or `1.5r` -- rational literal. Issue #841. Emits
       numerator + denominator as decimal-text fields (loader pins
       them in @nd_rat_num / @nd_rat_den). Prism stores both as
       reduced pm_integer_t. The text is exact: a part past the
       target's sp_int (`123456789012345678901234567890r`, `0.1r` with
       twenty-five digits) is a Bignum in CRuby, and `rat_big` says
       which (1 the numerator, 2 the denominator), so the literal is
       built as the big Rational instead of a saturated sp_int pair. */
    pm_rational_node_t *n = (pm_rational_node_t *)node;
    N("RationalNode");
    S("rat_num", pm_int_text(&n->numerator));
    S("rat_den", pm_int_text(&n->denominator));
    int big = (pm_int_overflows(&n->numerator) ? 1 : 0) | (pm_int_overflows(&n->denominator) ? 2 : 0);
    if (big) I("rat_big", big);
    break;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *n = (pm_string_node_t *)node;
    N("StringNode");
    emit_pm_string(id, "content", &n->unescaped);
    /* `fzl` only when the literal's file has the frozen pragma, so the AST
       text is byte-identical for programs without one. */
    if (sp_node_fsl(node)) I("fzl", 1);
    break;
  }
  case PM_INTERPOLATED_STRING_NODE: {
    pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
    N("InterpolatedStringNode");
    A("parts", &n->parts);
    if (sp_node_fsl(node)) I("fzl", 1);  /* adjacent-literal fold ("a" "b") */
    break;
  }
  case PM_EMBEDDED_STATEMENTS_NODE: {
    pm_embedded_statements_node_t *n = (pm_embedded_statements_node_t *)node;
    N("EmbeddedStatementsNode");
    R("statements", n->statements);
    break;
  }
  case PM_SYMBOL_NODE: {
    pm_symbol_node_t *n = (pm_symbol_node_t *)node;
    N("SymbolNode");
    emit_pm_string(id, "value", &n->unescaped);
    break;
  }
  case PM_TRUE_NODE:
    N("TrueNode");
    break;
  case PM_FALSE_NODE:
    N("FalseNode");
    break;
  case PM_NIL_NODE:
    N("NilNode");
    break;
  case PM_SELF_NODE:
    N("SelfNode");
    break;
  case PM_ARRAY_NODE: {
    pm_array_node_t *n = (pm_array_node_t *)node;
    N("ArrayNode");
    A("elements", &n->elements);
    break;
  }
  case PM_HASH_NODE: {
    pm_hash_node_t *n = (pm_hash_node_t *)node;
    N("HashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_ASSOC_NODE: {
    pm_assoc_node_t *n = (pm_assoc_node_t *)node;
    N("AssocNode");
    R("key", n->key);
    /* Hash shorthand `{ x: }` lowers to an AssocNode whose value is a
       PM_IMPLICIT_NODE. The top-level PM_IMPLICIT_NODE case below
       handles the unwrap by recursing into n->value at the same id
       slot, so the codegen never sees the implicit wrapper here. */
    R("value", n->value);
    break;
  }
  case PM_KEYWORD_HASH_NODE: {
    pm_keyword_hash_node_t *n = (pm_keyword_hash_node_t *)node;
    N("KeywordHashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_RANGE_NODE: {
    pm_range_node_t *n = (pm_range_node_t *)node;
    N("RangeNode");
    R("left", n->left);
    R("right", n->right);
    /* PM_RANGE_FLAGS_EXCLUDE_END: nt_range_excl tells `..` (inclusive)
       from `...` (exclusive). */
    I("flags", n->base.flags);
    break;
  }
  case PM_IF_NODE: {
    pm_if_node_t *n = (pm_if_node_t *)node;
    N("IfNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_ELSE_NODE: {
    pm_else_node_t *n = (pm_else_node_t *)node;
    N("ElseNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNLESS_NODE: {
    pm_unless_node_t *n = (pm_unless_node_t *)node;
    N("UnlessNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHILE_NODE: {
    pm_while_node_t *n = (pm_while_node_t *)node;
    N("WhileNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    /* PM_LOOP_FLAGS_BEGIN_MODIFIER: begin..end while form, which is a
       post-test loop (body runs at least once). nt_loop_post tells the codegen
       to emit `do {} while (cond);` rather than `while (cond) {}`. */
    I("flags", n->base.flags);
    break;
  }
  case PM_UNTIL_NODE: {
    pm_until_node_t *n = (pm_until_node_t *)node;
    N("UntilNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    I("flags", n->base.flags);
    break;
  }
  case PM_FOR_NODE: {
    pm_for_node_t *n = (pm_for_node_t *)node;
    N("ForNode");
    R("index", n->index);
    R("collection", n->collection);
    R("statements", n->statements);
    break;
  }
  case PM_CASE_NODE: {
    pm_case_node_t *n = (pm_case_node_t *)node;
    N("CaseNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    N("CaseMatchNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHEN_NODE: {
    pm_when_node_t *n = (pm_when_node_t *)node;
    N("WhenNode");
    A("conditions", &n->conditions);
    R("statements", n->statements);
    break;
  }
  case PM_IN_NODE: {
    pm_in_node_t *n = (pm_in_node_t *)node;
    N("InNode");
    R("pattern", n->pattern);
    R("statements", n->statements);
    break;
  }
  case PM_BEGIN_NODE: {
    pm_begin_node_t *n = (pm_begin_node_t *)node;
    N("BeginNode");
    R("statements", n->statements);
    R("rescue_clause", n->rescue_clause);
    R("ensure_clause", n->ensure_clause);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_ENSURE_NODE: {
    pm_ensure_node_t *n = (pm_ensure_node_t *)node;
    N("EnsureNode");
    R("statements", n->statements);
    break;
  }
  case PM_RESCUE_NODE: {
    pm_rescue_node_t *n = (pm_rescue_node_t *)node;
    N("RescueNode");
    A("exceptions", &n->exceptions);
    R("reference", n->reference);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_RESCUE_MODIFIER_NODE: {
    pm_rescue_modifier_node_t *n = (pm_rescue_modifier_node_t *)node;
    N("RescueModifierNode");
    R("expression", n->expression);
    R("rescue_expression", n->rescue_expression);
    break;
  }
  case PM_RETURN_NODE: {
    pm_return_node_t *n = (pm_return_node_t *)node;
    N("ReturnNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BREAK_NODE: {
    pm_break_node_t *n = (pm_break_node_t *)node;
    N("BreakNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_NEXT_NODE: {
    pm_next_node_t *n = (pm_next_node_t *)node;
    N("NextNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_RETRY_NODE:
    N("RetryNode");
    break;
  case PM_YIELD_NODE: {
    pm_yield_node_t *n = (pm_yield_node_t *)node;
    N("YieldNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BLOCK_NODE: {
    pm_block_node_t *n = (pm_block_node_t *)node;
    N("BlockNode");
    if (!n->opening_loc.start) I("sym_proc_block", 1);  /* an &:name block (sp_sym_proc_block) has no `{` */
    /* Block-local variables (params + first-assigned-inside): Ruby scoping
       makes non-param locals FRESH on every block invocation; codegen needs
       the list to reset them per iteration in fused loops. */
    out_block_locals(id, &n->locals);
    /* Serialize block parameters */
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        int bpid = node_counter++;
        nt_put_type(out_nt, bpid, "BlockParametersNode");
        if (in_bi) emit_int(bpid, "node_bi", g_bi_base + 1);
        if (bp->parameters) {
          emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        }
        nt_put_ref(out_nt, id, "parameters", bpid);
      }
else if (PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
        pm_numbered_parameters_node_t *np = (pm_numbered_parameters_node_t *)n->parameters;
        int npid = node_counter++;
        nt_put_type(out_nt, npid, "NumberedParametersNode");
        if (in_bi) emit_int(npid, "node_bi", g_bi_base + 1);
        emit_int(npid, "maximum", np->maximum);
        nt_put_ref(out_nt, id, "parameters", npid);
      }
else {
        R("parameters", n->parameters);
      }
    }
    R("body", n->body);
    break;
  }
  case PM_PARAMETERS_NODE: {
    pm_parameters_node_t *n = (pm_parameters_node_t *)node;
    N("ParametersNode");
    A("requireds", &n->requireds);
    A("optionals", &n->optionals);
    A("keywords", &n->keywords);
    if (n->rest) R("rest", n->rest);
    if (n->block) R("block", n->block);
    A("posts", &n->posts);
    /* Surface keyword_rest so the new PM_NO_KEYWORDS_PARAMETER_NODE
       case below is reachable. Existing collect_params_str walker
       only acts on KeywordRestParameterNode (`**kw`), so the
       NoKeywordsParameter (`**nil`) marker passes through as a
       no-op which matches its compile-time semantics. */
    if (n->keyword_rest) R("keyword_rest", n->keyword_rest);
    break;
  }
  case PM_REQUIRED_PARAMETER_NODE:
  case PM_OPTIONAL_PARAMETER_NODE:
  case PM_REST_PARAMETER_NODE:
  case PM_BLOCK_PARAMETER_NODE:
  case PM_BLOCK_LOCAL_VARIABLE_NODE:
  case PM_KEYWORD_REST_PARAMETER_NODE:
  case PM_REQUIRED_KEYWORD_PARAMETER_NODE:
  case PM_OPTIONAL_KEYWORD_PARAMETER_NODE:
    flatten_parameter(id, node);
    break;
  case PM_BLOCK_ARGUMENT_NODE: {
    /* `&expr` in call argument position. Wraps the expression that
     * provides the proc to forward (typically a LocalVariableReadNode
     * for a `&block`-captured param, or a SymbolNode for `&:to_s`). */
    pm_block_argument_node_t *n = (pm_block_argument_node_t *)node;
    N("BlockArgumentNode");
    R("expression", n->expression);
    break;
  }
  case PM_PARENTHESES_NODE: {
    pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
    N("ParenthesesNode");
    R("body", n->body);
    break;
  }
  case PM_AND_NODE: {
    pm_and_node_t *n = (pm_and_node_t *)node;
    N("AndNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_OR_NODE: {
    pm_or_node_t *n = (pm_or_node_t *)node;
    N("OrNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_DEFINED_NODE: {
    pm_defined_node_t *n = (pm_defined_node_t *)node;
    N("DefinedNode");
    R("value", n->value);
    break;
  }
  case PM_SOURCE_LINE_NODE: {
    N("SourceLineNode");
    int32_t line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);
    /* the line in the file it was written in: the buffer line counts every
       required file spliced in above it */
    if (sp_line_map_n > 0 && line >= 1 && line <= sp_line_map_n && sp_line_orig[line] > 0)
      line = sp_line_orig[line];
    I("start_line", (long long)line);
    break;
  }
  case PM_SOURCE_FILE_NODE: {
    /* `__FILE__`. The entry script answers the path spinel was given; a
       required file answers
       its own absolute path, which the source map attributes the node to
       (#4839). */
    N("SourceFileNode");
    { const char *rf = sp_node_required_file(node);
      if (rf) {
        char *ap = sp_required_file_path(rf);
        emit_str(id, "content", ap);
        free(ap);
      }
      else emit_str(id, "content", g_source_file); }
    if (sp_node_fsl(node)) I("fzl", 1);  /* __FILE__ is a literal of its file */
    break;
  }
  case PM_SOURCE_ENCODING_NODE: {
    /* `__ENCODING__`. Spinel sources are UTF-8; codegen returns a
       small Encoding value for Ruby-compatible `.class` / `.name`. */
    N("SourceEncodingNode");
    break;
  }
  case PM_IMPLICIT_NODE: {
    /* Wraps an implicit value reference, e.g. the value side of a
       hash-shorthand `{x:}`. Lowers to its inner value at the same
       id slot so the codegen never sees the implicit wrapper. Covers
       PM_IMPLICIT_NODE in any context (AssocNode value, kwarg
       shorthand inside KeywordHashNode, future Prism evolutions). */
    pm_implicit_node_t *n = (pm_implicit_node_t *)node;
    node_counter--;
    return flatten(n->value);
  }
  case PM_MISSING_NODE: {
    /* Prism emits MissingNode as an error-recovery placeholder. main()
       bails out before flatten() runs when parser.error_list is
       non-empty, so reaching this case means Prism produced a
       MissingNode without flagging an error -- a contract violation we
       surface clearly rather than silently miscompile. */
    fprintf(stderr, "spinel_parse: internal error: MissingNode reached flatten() at byte offset %td; parse error not in error_list\n",
            node->location.start - g_parser->start);
    exit(1);
  }
  case PM_SHAREABLE_CONSTANT_NODE: {
    /* `# shareable_constant_value: literal` magic comment that wraps
       a constant write. Spinel has no Ractor support, so the
       shareability state is a no-op. We lower at parse time by
       flattening the inner write directly into THIS node slot and
       discarding the wrapper. Many later codegen scanner passes
       look for ConstantWriteNode at the top level of statements;
       lowering here lets all of them work without modification. */
    pm_shareable_constant_node_t *n = (pm_shareable_constant_node_t *)node;
    /* Re-flatten the inner write at *this* id by rewinding the counter. */
    node_counter--;
    return flatten(n->write);
  }
  case PM_SPLAT_NODE: {
    pm_splat_node_t *n = (pm_splat_node_t *)node;
    N("SplatNode");
    R("expression", n->expression);
    break;
  }
  case PM_ASSOC_SPLAT_NODE: {
    /* `**h` in argument position -- splice a hash's entries as
       keyword args. Wraps the inner hash expression; codegen
       handles the expansion at call sites. Issue #917. */
    pm_assoc_splat_node_t *n = (pm_assoc_splat_node_t *)node;
    N("AssocSplatNode");
    R("value", n->value);
    break;
  }
  case PM_SUPER_NODE: {
    pm_super_node_t *n = (pm_super_node_t *)node;
    N("SuperNode");
    R("arguments", n->arguments);
    R("block", n->block);
    break;
  }
  case PM_FORWARDING_SUPER_NODE: {
    pm_forwarding_super_node_t *n = (pm_forwarding_super_node_t *)node;
    N("ForwardingSuperNode");
    R("block", (pm_node_t *)n->block);
    break;
  }
  case PM_MULTI_WRITE_NODE: {
    pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
    N("MultiWriteNode");
    A("lefts", &n->lefts);
    if (n->rest) R("rest", n->rest);
    A("rights", &n->rights);
    R("value", n->value);
    break;
  }
  case PM_IMPLICIT_REST_NODE:
    N("ImplicitRestNode");
    break;
  /* `def foo(...)` forwarding parameter and `bar(...)` forwarding
     arguments. No payload: `...` has no explicit names. analyze/codegen
     lower these into synthetic `*args, **kw, &block` slots (issue
     #1288). */
  case PM_FORWARDING_PARAMETER_NODE:
    N("ForwardingParameterNode");
    break;
  case PM_FORWARDING_ARGUMENTS_NODE:
    N("ForwardingArgumentsNode");
    break;
  case PM_LAMBDA_NODE: {
    pm_lambda_node_t *n = (pm_lambda_node_t *)node;
    N("LambdaNode");
    out_block_locals(id, &n->locals);
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        if (bp->parameters) {
          R("parameters", bp->parameters);
        }
      }
else if (PM_NODE_TYPE(n->parameters) != PM_NUMBERED_PARAMETERS_NODE) {
        R("parameters", n->parameters);
      }
    }
    if (n->body) R("body", n->body);
    break;
  }
  case PM_X_STRING_NODE: {
    pm_x_string_node_t *n = (pm_x_string_node_t *)node;
    N("XStringNode");
    emit_pm_string(id, "content", &n->unescaped);
    break;
  }
  case PM_INTERPOLATED_X_STRING_NODE: {
    pm_interpolated_x_string_node_t *n = (pm_interpolated_x_string_node_t *)node;
    N("InterpolatedXStringNode");
    A("parts", &n->parts);
    break;
  }
  case PM_REGULAR_EXPRESSION_NODE: {
    pm_regular_expression_node_t *n = (pm_regular_expression_node_t *)node;
    N("RegularExpressionNode");
    emit_pm_string(id, "unescaped", &n->unescaped);
    /* Emit Prism's regex flags so the codegen can pass /i, /x, /m
       through to the engine (nt_regexp_opts). */
    I("flags", n->base.flags);
    break;
  }
  case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {
    /* `/foo_#{x}/`. Same shape as InterpolatedStringNode -- carries
       `parts` -- plus a flags integer matching RegularExpressionNode.
       Codegen builds the pattern string via compile_interpolated and
       feeds it to sp_re_runtime_compile at execution time. */
    pm_interpolated_regular_expression_node_t *n = (pm_interpolated_regular_expression_node_t *)node;
    N("InterpolatedRegularExpressionNode");
    A("parts", &n->parts);
    I("flags", n->base.flags);
    break;
  }
  case PM_NUMBERED_REFERENCE_READ_NODE: {
    pm_numbered_reference_read_node_t *n = (pm_numbered_reference_read_node_t *)node;
    N("NumberedReferenceReadNode");
    I("number", n->number);
    break;
  }
  case PM_MATCH_WRITE_NODE: {
    pm_match_write_node_t *n = (pm_match_write_node_t *)node;
    N("MatchWriteNode");
    R("call", n->call);
    A("targets", &n->targets);  /* LocalVariableTargetNode per named group */
    break;
  }
  case PM_MATCH_REQUIRED_NODE: {
    /* Rightward assignment: `expr => var` (Ruby 3.0+). When the
       pattern is a single LocalVariableTargetNode, this is just
       `var = expr` and we lower it to a LocalVariableWriteNode so
       the codegen reuses the regular assignment path. Full pattern
       matching (array / hash patterns, pinned vars) is out of scope
       and falls through to the unknown-node passthrough. */
    pm_match_required_node_t *n = (pm_match_required_node_t *)node;
    if (n->pattern && PM_NODE_TYPE_P(n->pattern, PM_LOCAL_VARIABLE_TARGET_NODE)) {
      pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->pattern;
      N("LocalVariableWriteNode");
      NAME("name", t->name);
      R("value", n->value);
    }
else {
      N("MatchRequiredNode");
      R("value", n->value);
      R("pattern", n->pattern);
    }
    break;
  }
  case PM_MATCH_PREDICATE_NODE: {
    /* `expr in pattern`: a boolean one-line pattern test (Ruby 3.0+). */
    pm_match_predicate_node_t *n = (pm_match_predicate_node_t *)node;
    N("MatchPredicateNode");
    R("value", n->value);
    R("pattern", n->pattern);
    break;
  }
  case PM_ALTERNATION_PATTERN_NODE: {
    pm_alternation_pattern_node_t *n = (pm_alternation_pattern_node_t *)node;
    N("AlternationPatternNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_ARRAY_PATTERN_NODE: {
    /* `case x in [a, b, c]`. requireds is the list of fixed-position
       sub-patterns. rest / posts cover `[a, *, c]`. constant covers
       `Foo[a, b]` (Foo() destructuring). Minimal initial support:
       emit requireds as a node-id array; rest / posts / constant
       handled later when the codegen path catches up. Issue #669. */
    pm_array_pattern_node_t *n = (pm_array_pattern_node_t *)node;
    N("ArrayPatternNode");
    R("constant", n->constant);
    A("requireds", &n->requireds);
    R("rest", n->rest);
    A("posts", &n->posts);
    break;
  }
  case PM_HASH_PATTERN_NODE: {
    /* `case x in {a:, b: 2}`. elements is a list of AssocNodes
       (key + sub-pattern); a `:b:` shorthand binds the key's symbol
       as a same-named LV. Constant covers `Foo[a:]` destructuring
       (not yet handled by codegen). Rest covers `**rest` (also not
       yet handled). Issue #805. */
    pm_hash_pattern_node_t *n = (pm_hash_pattern_node_t *)node;
    N("HashPatternNode");
    R("constant", n->constant);
    A("elements", &n->elements);
    R("rest", n->rest);
    break;
  }
  case PM_FIND_PATTERN_NODE: {
    /* `case x in [*a, b, *c]` -- find-anywhere pattern with leading
       and trailing wildcards plus required middle elements.
       Currently surfaces all three slots so the codegen can match
       on length + extract requireds; the find-anywhere semantics
       (variable-position match) are not yet implemented but the
       shape no longer drops to UnsupportedNode. Issue #805. */
    pm_find_pattern_node_t *n = (pm_find_pattern_node_t *)node;
    N("FindPatternNode");
    R("constant", n->constant);
    R("left", (pm_node_t *)n->left);
    A("requireds", &n->requireds);
    R("right", n->right);
    break;
  }
  case PM_CAPTURE_PATTERN_NODE: {
    /* `case x in pat => var` -- match `pat`, bind matched value to
       `var` (LocalVariableTargetNode). Issue #884. */
    pm_capture_pattern_node_t *n = (pm_capture_pattern_node_t *)node;
    N("CapturePatternNode");
    R("value", (pm_node_t *)n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_PINNED_EXPRESSION_NODE: {
    /* `case x in ^(expr)`. The pinned expression is evaluated at
       match time and compared by `==` against the scrutinee. */
    pm_pinned_expression_node_t *n = (pm_pinned_expression_node_t *)node;
    N("PinnedExpressionNode");
    R("expression", n->expression);
    break;
  }
  case PM_PINNED_VARIABLE_NODE: {
    /* `case x in ^var`. The wrapped variable can be any read-shape
       node prism emits for `^var` -- LocalVariableReadNode,
       InstanceVariableReadNode, ClassVariableReadNode,
       GlobalVariableReadNode, ConstantReadNode,
       NumberedReferenceReadNode, BackReferenceReadNode. Routed to
       the same `@nd_expression` slot as PinnedExpressionNode so the
       codegen treats them uniformly -- both are "match the scrutinee
       by `==` against this expression". */
    pm_pinned_variable_node_t *n = (pm_pinned_variable_node_t *)node;
    N("PinnedVariableNode");
    R("expression", n->variable);
    break;
  }
  case PM_NUMBERED_PARAMETERS_NODE: {
    pm_numbered_parameters_node_t *n = (pm_numbered_parameters_node_t *)node;
    N("NumberedParametersNode");
    I("maximum", n->maximum);
    break;
  }
  case PM_ARGUMENTS_NODE: {
    pm_arguments_node_t *n = (pm_arguments_node_t *)node;
    N("ArgumentsNode");
    A("arguments", &n->arguments);
    break;
  }
  case PM_BLOCK_PARAMETERS_NODE: {
    pm_block_parameters_node_t *n = (pm_block_parameters_node_t *)node;
    N("BlockParametersNode");
    if (n->parameters) R("parameters", n->parameters);
    break;
  }
  case PM_IT_PARAMETERS_NODE:
    /* Ruby 3.4 implicit `it` is semantically `_1` -- lower to a
       NumberedParametersNode so the codegen's existing
       NumberedParametersNode arity path (get_block_param) handles it
       transparently. The block body's `it` references separately
       become PM_IT_LOCAL_VARIABLE_READ_NODE, also lowered below. */
    N("NumberedParametersNode");
    I("maximum", 1);
    break;
  case PM_IT_LOCAL_VARIABLE_READ_NODE:
    /* `it` inside the block body. Lowered to a regular
       LocalVariableReadNode named "_1" so it pairs with the
       lowered NumberedParametersNode { maximum: 1 } above. */
    N("LocalVariableReadNode");
    S("name", "_1");
    break;
  case PM_INTERPOLATED_SYMBOL_NODE: {
    /* `:"foo_#{x}"`. Carries `parts` like InterpolatedStringNode;
       codegen builds the string the same way and uses it directly --
       Spinel treats dynamic symbols as their assembled string value
       since it doesn't intern non-literal symbols. */
    pm_interpolated_symbol_node_t *n = (pm_interpolated_symbol_node_t *)node;
    N("InterpolatedSymbolNode");
    A("parts", &n->parts);
    break;
  }
  case PM_REDO_NODE:
    /* `redo`. Re-run the current iteration of the enclosing loop
       without re-evaluating the loop guard or advancing the
       iterator. Codegen emits a labeled `goto` to a label installed
       at the top of the iteration body. */
    N("RedoNode");
    break;
  case PM_BACK_REFERENCE_READ_NODE: {
    /* `$&`, `$~`, `$'`, $`. Spinel populates sp_re_match_str / _pre /
       _post during regex matches alongside sp_re_captures (which the
       NumberedReferenceReadNode arm already reads). */
    pm_back_reference_read_node_t *n = (pm_back_reference_read_node_t *)node;
    N("BackReferenceReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_MULTI_TARGET_NODE: {
    /* Nested LHS in destructuring multi-assign:
       `a, (b, c), d = 1, [2, 3], 4`. The inner (b, c) parenthesized
       group becomes a MultiTargetNode that recursively unpacks its
       slot of the RHS into the inner targets. */
    pm_multi_target_node_t *n = (pm_multi_target_node_t *)node;
    N("MultiTargetNode");
    A("lefts", &n->lefts);
    if (n->rest) R("rest", n->rest);
    A("rights", &n->rights);
    break;
  }
  case PM_EMBEDDED_VARIABLE_NODE: {
    /* `"foo #@bar"` shorthand for `"foo #{@bar}"`. The cleanest
       implementation is parser-side lowering: synthesize an
       EmbeddedStatementsNode wrapping a StatementsNode whose single
       body element is the variable read. The existing interpolation
       path then handles it without any codegen change.

       Same lowering trick as PM_IT_LOCAL_VARIABLE_READ_NODE -- emit
       a different node type at the same `id` slot so parent walks
       (parts list of the surrounding InterpolatedString) keep the
       slot unchanged. The wrapped StatementsNode gets a fresh id
       allocated via node_counter++. */
    pm_embedded_variable_node_t *n = (pm_embedded_variable_node_t *)node;
    N("EmbeddedStatementsNode");
    int var_id = flatten(n->variable);
    int stmts_id = node_counter++;
    nt_put_type(out_nt, stmts_id, "StatementsNode");
    if (in_bi) emit_int(stmts_id, "node_bi", g_bi_base + 1);
    int *body = malloc(sizeof(int));
    if (!body) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    body[0] = var_id;
    nt_put_arr(out_nt, stmts_id, "body", body, 1);
    nt_put_ref(out_nt, id, "statements", stmts_id);
    break;
  }
  case PM_ALIAS_METHOD_NODE: {
    /* `alias new old` -- compile-time method-name aliasing inside a
       class body. new_name and old_name are nodes representing the
       names (typically SymbolNode literals; InterpolatedSymbolNode is
       tolerated and silently skipped by the codegen helper). Spinel
       registers a duplicate method-table entry under the new name
       pointing to the same body, so dispatch on `.greet` lands on the
       same C function as `.hello`. */
    pm_alias_method_node_t *n = (pm_alias_method_node_t *)node;
    N("AliasMethodNode");
    R("new_name", n->new_name);
    R("old_name", n->old_name);
    break;
  }
  case PM_POST_EXECUTION_NODE: {
    /* `END { ... }`. CRuby runs END blocks in REVERSE registration
       order at program exit. Spinel emits each as a static C
       function and registers them via atexit() at main() startup --
       atexit naturally invokes handlers LIFO, matching CRuby. */
    pm_post_execution_node_t *n = (pm_post_execution_node_t *)node;
    N("PostExecutionNode");
    R("statements", n->statements);
    break;
  }
  case PM_PRE_EXECUTION_NODE: {
    /* `BEGIN { ... }`. CRuby runs all BEGIN blocks in source order
       BEFORE any other top-level statements. Spinel collects the
       bodies during a pre-pass and emits them at the very top of
       main() in source-encounter order. */
    pm_pre_execution_node_t *n = (pm_pre_execution_node_t *)node;
    N("PreExecutionNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNDEF_NODE: {
    /* `undef foo, bar` inside a class body. CRuby raises NoMethodError
       at runtime when an undef'd method is called; Spinel's AOT model
       resolves dispatch at compile time, so we record the undefs but
       leave compile-time enforcement of "calling an undef'd method
       fails" to a future pass. */
    pm_undef_node_t *n = (pm_undef_node_t *)node;
    N("UndefNode");
    A("names", &n->names);
    break;
  }
  case PM_ALIAS_GLOBAL_VARIABLE_NODE: {
    /* `alias $copy $orig` -- compile-time gvar aliasing. The
       new_name and old_name slots are GlobalVariableReadNodes whose
       `name` field carries the literal $-prefixed name. Spinel
       resolves $copy to $orig everywhere the alias is in scope, so
       the C output uses one storage slot for both. */
    pm_alias_global_variable_node_t *n = (pm_alias_global_variable_node_t *)node;
    N("AliasGlobalVariableNode");
    R("new_name", n->new_name);
    R("old_name", n->old_name);
    break;
  }
  default: {
    /* Previously emitted UnknownNode_<n> which silently degraded to
       "0" in codegen. Now emit a hard-error sentinel carrying the
       Prism node kind name (in human-friendly Ruby vocabulary) + the
       source line so codegen refuses to compile and tells the user
       exactly what's wrong. */
    char pretty[128];
    size_t plen = prism_kind_to_pascal(pm_node_type_to_str(t), pretty, sizeof(pretty));
    int32_t line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);

    N("UnsupportedNode");
    emit_bytes(id, "kind", pretty, plen);
    emit_int(id, "source_line", (long long)line);
    break;
  }
  }

#undef N
#undef S
#undef I
#undef F
#undef R
#undef A
#undef NAME

  g_owner = owner;
  return id;
}

/* ---- require_relative resolution ---- */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long len = ftell(f);
  /* Issue #768: ftell returning -1 (stream error) used to wrap len + 1
     to 0, malloc(0) returns NULL, then fread(NULL, 1, SIZE_MAX, f)
     tried to read 16 exabytes. Bail cleanly. */
  if (len < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t nread = fread(buf, 1, (size_t)len, f);
  /* Issue #763: detect short read and bail rather than processing
     a truncated source file silently. */
  if (nread != (size_t)len) { free(buf); fclose(f); return NULL; }
  buf[nread] = '\0';
  fclose(f);
  return buf;
}

/* Track files already inlined so duplicate requires/require_relatives in
   different files don't re-emit (and re-define structs/classes) the same
   content. Dynamic so we don't silently drop entries on large projects. */
static char **sp_included_paths = NULL;
static int sp_included_count = 0;
static int sp_included_cap = 0;

/* Whether `src` references the Set constant (identifier-boundary scan: the
   char before is not part of an identifier or a `::`/`.` qualifier, the char
   after doesn't extend the word -- so `Set[`, `Set.new`, `Set(` hit while
   `Settings`, `OffSet`, `Foo::Set` don't) or calls `.to_set`. Drives the
   implicit `require "set"` splice below. */
/* The scan runs over CODE, not over the file. It was a plain strstr over the
   whole text, so the word in a comment counted: `# Set` at the top of a
   hello-world spliced the whole of set.rb in and took its generated C from 41
   lines to 959 (#4411). On the reporter's program it did worse than waste --
   a sentence in a benchmark's header comment linked Set, which changed the
   lowering of an unrelated closure and with it the ANSWER (#4410).

   Skipping is deliberately conservative, because a false negative drops a
   `require` the program needs while a false positive only wastes: an
   interpolation inside a double-quoted string is scanned as the code it is,
   and every literal form not handled here -- %w, heredocs, regexps -- stays
   code and keeps the old over-linking behaviour. */
static int sp_set_word_at(const char *src, const char *p) {
  char prev = p == src ? 0 : p[-1];
  char next = p[3];
  int prev_ok = prev == 0 ||
                (!((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                   (prev >= '0' && prev <= '9') || prev == '_' || prev == ':' || prev == '.'));
  int next_ok = !((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                  (next >= '0' && next <= '9') || next == '_');
  return prev_ok && next_ok;
}
/* Whether hit(src, p) answers true at some p in CODE: comments, =begin
   blocks and the text of quoted strings are passed over, and a #{...} inside
   double quotes is scanned as the code it is. */
static int sp_src_code_any(const char *src, int (*hit)(const char *src, const char *p)) {
  const char *p = src;
  int bol = 1;   /* at the beginning of a line, for =begin/=end */
  while (*p) {
    if (bol && strncmp(p, "=begin", 6) == 0) {
      const char *e = strstr(p, "\n=end");
      if (!e) return 0;          /* unterminated: the rest of the file is comment */
      p = e + 5;
      continue;
    }
    bol = 0;
    if (*p == '#') {             /* comment to end of line */
      while (*p && *p != '\n') p++;
      continue;
    }
    if (*p == '\'') {            /* single quotes do not interpolate: skip whole */
      p++;
      while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
      if (*p) p++;
      continue;
    }
    if (*p == '"') {             /* skip the text, but scan #{...} as code */
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (p[0] == '#' && p[1] == '{') {
          const char *q = p + 2;
          int depth = 1;
          while (*q && depth) { if (*q == '{') depth++; else if (*q == '}') depth--; q++; }
          for (const char *r = p + 2; r < q; r++)
            if (hit(src, r)) return 1;
          p = q;
          continue;
        }
        p++;
      }
      if (*p) p++;
      continue;
    }
    if (hit(src, p)) return 1;
    if (*p == '\n') bol = 1;
    p++;
  }
  return 0;
}
static int sp_set_hit(const char *src, const char *p) {
  return (strncmp(p, "Set", 3) == 0 && sp_set_word_at(src, p)) || strncmp(p, ".to_set", 7) == 0;
}
static int source_references_set(const char *src) {
  return sp_src_code_any(src, sp_set_hit);
}

/* Whether `src` references `IO::Buffer` in CODE, with the same
   comment/string skipping as source_references_set above. The needle is
   already two qualified segments, so the boundary check is only that the
   match is not embedded in a longer identifier (`IO::BufferPool`) or
   preceded by one (`MyIO::Buffer` still matches -- a false positive only
   splices the declarations needlessly). Drives the implicit
   `require "io/buffer"` splice below: CRuby provides IO::Buffer without a
   require, so Spinel mirrors it the way it mirrors Set. */
static int sp_iob_word_at(const char *src, const char *p) {
  char prev = p == src ? 0 : p[-1];
  char next = p[10];   /* strlen("IO::Buffer") */
  int prev_ok = !((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                  (prev >= '0' && prev <= '9') || prev == '_' || prev == '.');
  int next_ok = !((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                  (next >= '0' && next <= '9') || next == '_');
  return prev_ok && next_ok;
}
static int sp_iob_hit(const char *src, const char *p) {
  return strncmp(p, "IO::Buffer", 10) == 0 && sp_iob_word_at(src, p);
}
static int source_references_io_buffer(const char *src) {
  return sp_src_code_any(src, sp_iob_hit);
}

/* Whether `src` calls Time.parse or Time.strptime in CODE (the skipping of
   sp_src_code_any). `Time` starts a word that no `.` or `::` qualifies, so
   DateTime.parse and Foo::Time.parse do not count, though a top-level
   ::Time.parse does; the method name ends there, so Time.parser does not.
   `Time::parse` and a `.` on the next line count too. Drives the time.rb
   splice below; a receiver held in a variable (`k = Time; k.parse(s)`) is
   not seen. */
static int sp_req_ident_char(char ch);
static int sp_time_parser_hit(const char *src, const char *p) {
  if (strncmp(p, "Time", 4) != 0) return 0;
  const char *b = p;
  if (b - src >= 2 && b[-1] == ':' && b[-2] == ':') b -= 2;   /* ::Time */
  if (b > src && (sp_req_ident_char(b[-1]) || b[-1] == '.' || b[-1] == ':')) return 0;
  const char *q = p + 4;
  while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
  if (q[0] == '&' && q[1] == '.') q += 2;
  else if (q[0] == '.') q++;
  else if (q[0] == ':' && q[1] == ':') q += 2;
  else return 0;
  while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
  size_t n = strncmp(q, "parse", 5) == 0 ? 5 : strncmp(q, "strptime", 8) == 0 ? 8 : 0;
  return n && !sp_req_ident_char(q[n]);
}
static int source_calls_time_parser(const char *src) {
  return sp_src_code_any(src, sp_time_parser_hit);
}

/* ---- require-gate: features enabled by a `require "name"` ----
   SPINEL_REQUIRE_GATE: when set, a require-gated stdlib feature (stringio,
   io/console, ...) is provided only if its `require` textually appears in the
   program, matching CRuby's uninitialized-constant / NoMethodError. Default off
   keeps the historical always-available behaviour. The feature name is the
   require argument verbatim ("stringio", "io/console"). Marked as each plain
   require is resolved (file-backed or native); read at analyze/codegen time via
   sp_feature_enabled(). Definitions here (spinel_parse.c includes no project
   headers); declared extern in compiler.h. */
int g_require_gate = 0;
/* A String is shared by reference, as CRuby shares it (#6765): the one
   mode, with no switch to copy. The compiler's flag and the parser's read
   of a package's share declarations ask this one test; `--share-strings`
   is still accepted and changes nothing. */
int sp_share_strings_env(void) { return 1; }
int g_require_gate_cli = 0;   /* --require-gate */
static char **sp_req_feats = NULL;
static int sp_req_feats_n = 0;
static int sp_req_feats_cap = 0;

static char **sp_grow_strs(char **a, int n, int *cap, int init) {
  if (n < *cap) return a;
  int nc = *cap == 0 ? init : *cap * 2;
  char **np = (char **)realloc(a, sizeof(char *) * nc);
  if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  *cap = nc;
  return np;
}

void sp_feature_mark(const char *name) {
  if (!name) return;
  for (int i = 0; i < sp_req_feats_n; i++)
    if (strcmp(sp_req_feats[i], name) == 0) return;
  sp_req_feats = sp_grow_strs(sp_req_feats, sp_req_feats_n, &sp_req_feats_cap, 64);
  sp_req_feats[sp_req_feats_n++] = strdup(name);
}

int sp_feature_enabled(const char *name) {
  if (!g_require_gate) return 1;   /* gate off: always provide (baseline) */
  if (!name) return 1;
  for (int i = 0; i < sp_req_feats_n; i++)
    if (strcmp(sp_req_feats[i], name) == 0) return 1;
  return 0;
}

/* Like sp_feature_enabled, but the require is mandatory REGARDLESS of the
   require-gate mode: true only when the program actually `require`d the
   feature. For stdlib whose classes CRuby only defines after the require
   (socket's TCPServer/TCPSocket) -- without this, the baseline mode would
   grow the constants unconditionally and diverge from CRuby's NameError. */
int sp_feature_required(const char *name) {
  if (!name) return 0;
  for (int i = 0; i < sp_req_feats_n; i++)
    if (strcmp(sp_req_feats[i], name) == 0) return 1;
  return 0;
}

/* Feature search roots from `-I <dir>` (like ruby -I). A plain `require "X"`
   that is not a bundled lib or native feature is looked up here, in order, as
   <root>/X.rb (CRuby single-file form) or <root>/X/<last>.rb (the colocated
   directory form, where one feature's sources share a directory). */
static char **sp_feature_roots = NULL;
static int sp_feature_roots_n = 0;
static int sp_feature_roots_cap = 0;

void sp_add_feature_root(const char *dir) {
  if (!dir) return;
  sp_feature_roots = sp_grow_strs(sp_feature_roots, sp_feature_roots_n, &sp_feature_roots_cap, 16);
  sp_feature_roots[sp_feature_roots_n++] = strdup(dir);
}

/* Resolve a path to its canonical form for dedup. realpath() returns NULL
   on missing files; in that case fall back to the literal path. */
/* Issue #749: realpath/_fullpath + strdup can both fail under OOM,
   leaving callers with a NULL pointer that goes to strcmp(NULL, ...)
   and segfaults. Fall back to a static empty string so downstream
   `strcmp(canonical, ...) == 0` consistently misses (the caller then
   treats this as "not already included" and proceeds with the file
   read, which fails through the normal LoadError path). */
static char *sp_canonical_path(const char *path) {
  if (!path) { char *e = strdup(""); return e ? e : NULL; }
  char *real = realpath(path, NULL);
  if (real) return real;
  char *d = strdup(path);
  if (d) return d;
  /* Last resort: return a static empty string to avoid NULL. The
     buffer is read-only by callers (passed to strcmp, then freed by
     the caller's free() -- so allocate a fresh empty string instead
     of returning a string literal). */
  return strdup("");
}

static int sp_rr_included = 0;
static int sp_path_already_included(const char *canonical) {
  if (!canonical) return 0;
  for (int i = 0; i < sp_included_count; i++) {
    if (sp_included_paths[i] && strcmp(sp_included_paths[i], canonical) == 0) return 1;
  }
  return 0;
}

static void sp_mark_path_included(const char *canonical) {
  sp_included_paths = sp_grow_strs(sp_included_paths, sp_included_count, &sp_included_cap, 16);
  sp_included_paths[sp_included_count++] = strdup(canonical);
}

/* Free the included-paths table at end of run. The process is short-lived,
   so this matters mostly for tools (leak checkers, embedders) that
   scrutinise end-of-run state. */
static void sp_includes_free(void) {
  for (int i = 0; i < sp_included_count; i++) {
    free(sp_included_paths[i]);
  }
  free(sp_included_paths);
  sp_included_paths = NULL;
  sp_included_count = 0;
  sp_included_cap = 0;
}

/* ---- Debug multi-file source map (g_emit_line only) -------------------
   require/require_relative are resolved by *textual* splicing into one
   buffer, so a node's Prism line is a line in that concatenated buffer and
   the original file boundaries are lost. For --debug we recover them: each
   spliced file's content is wrapped in PUSH/POP marker *comments* (Prism
   ignores comments, so the AST is unchanged), and a single pass over the
   final buffer reconstructs, per buffer line, which file and original line
   it came from. The stack accounting needs no original-line bookkeeping at
   splice time: a PUSH consumes the parent's (replaced) require line, while
   INSERT leaves that line in place. Content lines count within the child's
   own coordinates until POP. */
#define SP_PUSH_PREFIX "#<SPINEL_PUSH>"
#define SP_INSERT_PREFIX "#<SPINEL_INSERT>"
#define SP_POP_PREFIX "#<SPINEL_POP>"
/* A generated .rb names the source its lines came from (#7630):
   `#<SPINEL_SOURCE>greeting.html.erb:12` pins file and line for the lines
   after it, until the next marker or the end of the file it is in. */
#define SP_SOURCE_PREFIX "#<SPINEL_SOURCE>"

/* The byte ranges of the final buffer a builtins/ file was spliced into.
   A node inside one is stamped `node_bi`, so the names the compiler invents
   from nodes number a builtin's nodes apart from the program's, and each
   builtin method's apart from the others': a builtin that grows or shrinks
   then moves only its own names (comp_node_ord).
   Read off the splice markers, which every build keeps, not off the line
   map, which only a build with line maps has. */
static size_t *sp_bi_lo = NULL, *sp_bi_hi = NULL;
/* The files the require resolver took from the compiler's own builtins/
   directory: a splice is a builtin's by where it came from, not by a
   "/builtins/" somewhere in its path -- a program's own lib/builtins/x.rb
   is the program's. */
static char **sp_bi_paths = NULL;
static int sp_bi_npaths = 0;
static void sp_note_builtin_path(const char *path) {
  for (int i = 0; i < sp_bi_npaths; i++) if (!strcmp(sp_bi_paths[i], path)) return;
  char **np = realloc(sp_bi_paths, sizeof(char *) * (size_t)(sp_bi_npaths + 1));
  if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_bi_paths = np;
  sp_bi_paths[sp_bi_npaths++] = strdup(path);
}
static int sp_is_builtin_path(const char *p, size_t n) {
  for (int i = 0; i < sp_bi_npaths; i++)
    if (strlen(sp_bi_paths[i]) == n && !strncmp(sp_bi_paths[i], p, n)) return 1;
  return 0;
}
static int sp_bi_n = 0;
static const char *sp_bi_base = NULL;
static void sp_find_builtin_ranges(const char *src) {
  free(sp_bi_lo); free(sp_bi_hi); sp_bi_lo = sp_bi_hi = NULL; sp_bi_n = 0;
  sp_bi_base = src;
  size_t stk_lo[64]; int stk_bi[64]; int sp = 0, cap = 0;
  for (const char *line = src; *line; ) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    size_t pl = !strncmp(line, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) ? strlen(SP_PUSH_PREFIX)
              : !strncmp(line, SP_INSERT_PREFIX, strlen(SP_INSERT_PREFIX)) ? strlen(SP_INSERT_PREFIX) : 0;
    if (pl && sp < 64) {
      int bi = sp_is_builtin_path(line + pl, len - pl);
      stk_lo[sp] = (size_t)(line - src); stk_bi[sp] = bi; sp++;
    }
    else if (!strncmp(line, SP_POP_PREFIX, strlen(SP_POP_PREFIX)) && sp > 0) {
      sp--;
      if (stk_bi[sp]) {
        if (sp_bi_n == cap) {
          cap = cap ? cap * 2 : 8;
          sp_bi_lo = realloc(sp_bi_lo, sizeof(size_t) * (size_t)cap);
          sp_bi_hi = realloc(sp_bi_hi, sizeof(size_t) * (size_t)cap);
          if (!sp_bi_lo || !sp_bi_hi) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
        }
        sp_bi_lo[sp_bi_n] = stk_lo[sp]; sp_bi_hi[sp_bi_n] = (size_t)(line - src); sp_bi_n++;
      }
    }
    if (!eol) break;
    line = eol + 1;
  }
}
static int sp_in_builtin(const uint8_t *at) {
  if (!sp_bi_base || !at) return 0;
  size_t off = (size_t)((const char *)at - sp_bi_base);
  for (int i = 0; i < sp_bi_n; i++) if (off >= sp_bi_lo[i] && off < sp_bi_hi[i]) return 1;
  return 0;
}

static char **sp_file_table = NULL;  /* id -> path (declared above flatten) */
static int sp_file_count = 0, sp_file_cap = 0;

static int sp_intern_file(const char *path) {
  for (int i = 0; i < sp_file_count; i++)
    if (strcmp(sp_file_table[i], path) == 0) return i;
  sp_file_table = sp_grow_strs(sp_file_table, sp_file_count, &sp_file_cap, 8);
  char *dup_path = strdup(path);
  if (!dup_path) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_file_table[sp_file_count] = dup_path;
  return sp_file_count++;
}

/* In debug mode, wrap an included file's (already-resolved) content with
   PUSH <path> / POP marker lines. Takes ownership of `content`; returns it
   unchanged when not in debug mode. */
static char *sp_wrap_included(char *content, const char *path) {
  if (!g_src_map) return content;
  size_t clen = strlen(content);
  int need_nl = (clen > 0 && content[clen - 1] != '\n') ? 1 : 0;
  size_t total = strlen(SP_PUSH_PREFIX) + strlen(path) + 1 + clen + need_nl
               + strlen(SP_POP_PREFIX) + 1 + 1;
  char *w = malloc(total + 8);
  if (!w) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t o = (size_t)sprintf(w, SP_PUSH_PREFIX "%s\n", path);
  memcpy(w + o, content, clen); o += clen;
  if (need_nl) w[o++] = '\n';
  o += (size_t)sprintf(w + o, SP_POP_PREFIX "\n");
  w[o] = '\0';
  free(content);
  return w;
}

/* Reconstruct the line map from the final spliced buffer. toplevel is the
   path of the outermost file (interned as the initial frame). */
static void sp_build_line_map(const char *src, const char *toplevel) {
  size_t nlines = 1;
  for (const char *p = src; *p; p++) if (*p == '\n') nlines++;
  sp_line_map_n = (int)nlines;
  sp_line_file = (int *)calloc(nlines + 2, sizeof(int));
  sp_line_orig = (int *)calloc(nlines + 2, sizeof(int));
  sp_line_pop = (int *)calloc(nlines + 2, sizeof(int));
  sp_disp_file = (int *)calloc(nlines + 2, sizeof(int));
  sp_disp_line = (int *)calloc(nlines + 2, sizeof(int));
  int *stk_dfile = (int *)calloc(nlines + 2, sizeof(int));  /* a frame's pinned file id, 0 = none */
  int *stk_dline = (int *)calloc(nlines + 2, sizeof(int));

  int *stk_file = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_next = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_start = (int *)malloc(sizeof(int) * (nlines + 2));
  if (!sp_line_file || !sp_line_orig || !sp_line_pop || !sp_disp_file || !sp_disp_line || !stk_dfile || !stk_dline || !stk_file || !stk_next || !stk_start) {
    fprintf(stderr, "spinel_parse: out of memory\n"); exit(1);
  }
  int sp = 0;
  stk_file[sp] = sp_intern_file(toplevel);
  stk_next[sp] = 1;

  int bl = 1;
  const char *line = src;
  while (1) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    size_t prefix_len = 0;
    if (strncmp(line, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) == 0) {
      /* The replaced require occupied one parent line: consume it. */
      if (sp >= 0) stk_next[sp] += 1;
      prefix_len = strlen(SP_PUSH_PREFIX);
    }
else if (strncmp(line, SP_INSERT_PREFIX, strlen(SP_INSERT_PREFIX)) == 0) {
      prefix_len = strlen(SP_INSERT_PREFIX);
    }
    if (prefix_len) {
      char pathbuf[1024];
      size_t plen = len - prefix_len;
      if (plen >= sizeof(pathbuf)) plen = sizeof(pathbuf) - 1;
      memcpy(pathbuf, line + prefix_len, plen);
      pathbuf[plen] = '\0';
      sp++;
      stk_file[sp] = sp_intern_file(pathbuf);
      stk_next[sp] = 1;
      stk_start[sp] = bl;
      stk_dfile[sp] = 0;
      /* marker line maps to nothing meaningful */
    }
else if (strncmp(line, SP_POP_PREFIX, strlen(SP_POP_PREFIX)) == 0) {
      for (int j = sp > 0 ? stk_start[sp] : bl; j < bl; j++) if (!sp_line_pop[j]) sp_line_pop[j] = bl;
      if (sp > 0) sp--;
    }
else if (len < 12 || strncmp(line + len - 12, "SPINEL_COND>", 12) != 0) {
      sp_line_file[bl] = stk_file[sp];
      sp_line_orig[bl] = stk_next[sp];
      stk_next[sp] += 1;
      if (strncmp(line, SP_SOURCE_PREFIX, strlen(SP_SOURCE_PREFIX)) == 0) {
        /* file:line, split at the last colon; anything else is a comment */
        char pathbuf[1024];
        size_t plen = len - strlen(SP_SOURCE_PREFIX);
        if (plen >= sizeof(pathbuf)) plen = sizeof(pathbuf) - 1;
        memcpy(pathbuf, line + strlen(SP_SOURCE_PREFIX), plen);
        pathbuf[plen] = '\0';
        while (plen > 0 && (pathbuf[plen - 1] == '\r' || pathbuf[plen - 1] == ' ')) pathbuf[--plen] = '\0';
        char *colon = strrchr(pathbuf, ':');
        char *end = NULL;
        long ln = colon ? strtol(colon + 1, &end, 10) : 0;
        if (colon && colon > pathbuf && end && end != colon + 1 && *end == '\0' && ln > 0 && ln < 1000000000) {
          *colon = '\0';
          stk_dfile[sp] = sp_intern_file(pathbuf);
          stk_dline[sp] = (int)ln;
        }
      }
      else if (stk_dfile[sp]) {
        sp_disp_file[bl] = stk_dfile[sp];
        sp_disp_line[bl] = stk_dline[sp];
      }
    }
    bl++;
    if (!eol) break;
    line = eol + 1;
    if (*line == '\0') break;
  }
  free(stk_file);
  free(stk_next);
  free(stk_start);
  free(stk_dfile);
  free(stk_dline);
}

/* Lexically collapse "." and ".." path segments, like File.expand_path,
   without touching the filesystem. require_relative targets are formed as
   `dir + "/" + rel_path` and then have ".rb" appended; a rel_path ending in
   ".." -- e.g. `require_relative ".."`, which Ruby resolves to the parent
   directory's `<dir>.rb` (it normalizes `a/b/..` -> `a` then appends ".rb")
   -- would otherwise get ".rb" glued onto the literal ".." to form a bogus
   "...rb". (A ".." in the middle of a path happened to work only because the
   OS resolves it when the final file is opened; a trailing ".." cannot,
   since ".rb" makes it a literal "...rb" name.) Rewrites in place; the
   result is never longer than the input. */
static void sp_normalize_dots(char *path) {
  if (!path || !*path) return;
  int absolute = (path[0] == '/');
  size_t len = strlen(path);
  char *copy = (char *)malloc(len + 1);
  if (!copy) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(copy, path, len + 1);

  const char **seg = (const char **)malloc((len + 1) * sizeof(char *));
  size_t *seglen = (size_t *)malloc((len + 1) * sizeof(size_t));
  if (!seg || !seglen) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t n = 0;

  char *p = copy;
  while (*p) {
    while (*p == '/') p++;                 /* skip separators */
    if (!*p) break;
    char *s = p;
    while (*p && *p != '/') p++;
    size_t l = (size_t)(p - s);
    if (l == 1 && s[0] == '.') continue;   /* "." */
    if (l == 2 && s[0] == '.' && s[1] == '.') {
      if (n > 0 && !(seglen[n-1] == 2 && seg[n-1][0] == '.' && seg[n-1][1] == '.')) {
        n--;                               /* pop a real parent segment */
        continue;
      }
      if (absolute) continue;              /* cannot ascend past root */
      /* relative with nothing to pop: keep the ".." */
    }
    seg[n] = s; seglen[n] = l; n++;
  }

  char *w = path;
  if (absolute) *w++ = '/';
  for (size_t i = 0; i < n; i++) {
    if (i > 0) *w++ = '/';
    memcpy(w, seg[i], seglen[i]);
    w += seglen[i];
  }
  if (w == path) *w++ = '.';               /* empty relative result -> "." */
  *w = '\0';

  free(copy); free(seg); free(seglen);
}

/* Simple require_relative resolver: replace lines matching
   require_relative "path" with the file content. Files that have
   already been included once are silently skipped on subsequent
   requires (matching Ruby's load-once semantics). */
/* PUSH/POP wrap adds one marker line before and after (--debug only): pad the
   file's flag lines to match so the buffer stays aligned with the text. */
static void sp_fsl_pad_wrap(unsigned char **v, size_t *n) {
  unsigned char z = 0;
  if (!g_src_map) return;
  sp_fsl_splice(v, n, 0, 0, &z, 1);
  sp_fsl_splice(v, n, *n, 0, &z, 1);
}

/* 0-based line index of byte offset `at` in `text` (both resolvers splice at
   line starts, so this is the spliced line's index in the flag buffer). */
static size_t sp_fsl_line_at(const char *text, size_t at) {
  size_t line = 0;
  for (size_t i = 0; i < at; i++) if (text[i] == '\n') line++;
  return line;
}

/* One `require` / `require_relative` occurrence, located by a scan that knows
   where string literals, comments and heredoc bodies are -- so the word inside
   a fixture string is not mistaken for a real one.

   `kw` points at the keyword and `expr_end` just past the whole call (past the
   closing paren in the parenthesized form, past the closing quote otherwise).
   `margin` is 1 when the keyword starts the line. Only then may the inlined
   content take the statement's place: anywhere else -- indented inside a
   method or class body, in a condition, on an assignment's right-hand side --
   the library belongs at the top of the program (a module cannot be defined
   inside a method body, and a class nested in a reopened class is not the same
   class), with the call itself replaced by the value it answers. */
typedef struct {
  char *kw, *arg, *expr_end;
  size_t arg_len;
  int margin;
} SpReqHit;

static int sp_req_ident_char(char ch) {
  return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9');
}

/* The quoted argument of a require starting at `p` (just past the keyword),
   in either the bare or the parenthesized form, confined to one line. */
static int sp_req_parse_arg(char *p, char *line_end, SpReqHit *h) {
  int paren = 0;
  while (p < line_end && (*p == ' ' || *p == '\t')) p++;
  if (p < line_end && *p == '(') { paren = 1; p++; while (p < line_end && (*p == ' ' || *p == '\t')) p++; }
  if (p >= line_end || (*p != '"' && *p != '\'')) return 0;
  char q = *p++;
  char *arg = p;
  while (p < line_end && *p != q) {
    if (*p == '\\') p++;
    p++;
  }
  if (p >= line_end || *p != q) return 0;
  h->arg = arg;
  h->arg_len = (size_t)(p - arg);
  p++;                                  /* past the closing quote */
  if (paren) {
    while (p < line_end && (*p == ' ' || *p == '\t')) p++;
    if (p >= line_end || *p != ')') return 0;
    p++;
  }
  h->expr_end = p;
  return 1;
}

/* The first `word` call at or after `from`. Returns 0 when there is none. */
static int sp_require_find(char *buf, char *from, const char *word, SpReqHit *out) {
  size_t wlen = strlen(word);
  char hd_term[128];   /* the pending heredoc terminator, "" when not in one */
  hd_term[0] = 0;
  char *line = buf;
  while (*line) {
    char *line_end = strchr(line, '\n');
    if (!line_end) line_end = line + strlen(line);
    if (hd_term[0]) {
      /* inside a heredoc body: the terminator line ends it, nothing else counts */
      char *t = line;
      while (t < line_end && (*t == ' ' || *t == '\t')) t++;
      size_t tl = strlen(hd_term);
      if ((size_t)(line_end - t) == tl && strncmp(t, hd_term, tl) == 0) hd_term[0] = 0;
      line = (*line_end == '\n') ? line_end + 1 : line_end;
      continue;
    }
    for (char *p = line; p < line_end; p++) {
      if (*p == '#') break;                                  /* comment to end of line */
      if (*p == '"' || *p == '\'') {                         /* skip a string literal */
        char q = *p++;
        while (p < line_end && *p != q) { if (*p == '\\') p++; p++; }
        if (p >= line_end) break;                            /* unterminated: give up on the line */
        continue;
      }
      if (p[0] == '<' && p + 1 < line_end && p[1] == '<' &&
          (p == line || !sp_req_ident_char(p[-1]))) {        /* a heredoc opener */
        char *t = p + 2;
        if (t < line_end && (*t == '~' || *t == '-')) t++;
        char tq = 0;
        if (t < line_end && (*t == '"' || *t == '\'')) tq = *t++;
        char *ts = t;
        while (t < line_end && sp_req_ident_char(*t)) t++;
        if (t > ts && (!tq || (t < line_end && *t == tq))) {
          size_t tl = (size_t)(t - ts);
          if (tl < sizeof hd_term) { memcpy(hd_term, ts, tl); hd_term[tl] = 0; }
        }
        continue;
      }
      if (*p != word[0] || strncmp(p, word, wlen) != 0) continue;
      if (sp_req_ident_char(p[wlen])) continue;              /* require_relative vs require */
      /* `Kernel.require "x"` (`::Kernel.` too) is the bare require -- the
         deprecation proxies in activesupport spell it so; any other
         receiver makes it that object's method and not a require at all */
      char *kw = p;
      if (p > buf && p[-1] == '.') {
        char *q = p - 1;
        if (q - line >= 6 && strncmp(q - 6, "Kernel", 6) == 0 &&
            (q - 6 == line || !sp_req_ident_char(q[-7]))) {
          kw = q - 6;
          if (kw - line >= 2 && kw[-1] == ':' && kw[-2] == ':') kw -= 2;
        }
        else continue;
      }
      else if (p > buf && (sp_req_ident_char(p[-1]) || p[-1] == ':' ||
                           p[-1] == '@' || p[-1] == '$')) continue;
      if (p < from) continue;
      SpReqHit h;
      memset(&h, 0, sizeof h);
      if (!sp_req_parse_arg(p + wlen, line_end, &h)) continue;
      h.kw = kw;
      h.margin = (kw == line);
      *out = h;
      return 1;
    }
    line = (*line_end == '\n') ? line_end + 1 : line_end;
  }
  return 0;
}

/* Rebuild `*result` with `content` inlined ahead of the top-level statement
   the call sits in (sp_toplevel_stmt_start) and the call itself replaced by
   `value`, so the statement around the require -- a
   condition, an assignment, a method body -- survives intact. */
/* The start of the top-level statement a position sits inside: the nearest
   line at or above it that begins, at the margin, something other than the
   continuation of a statement already open (end / else / rescue / a closing
   bracket / a comment). A require nested in that statement -- inside an `if`,
   a `begin`, a method body -- is spliced there, so the program keeps running
   top to bottom: the library loads where the code needing it stands, not at
   the very start, ahead of the definitions its own body calls (ffi-rzmq-core
   requires libzmq4 under `if LibZMQ.version4?`, and libzmq4 reopens LibZMQ). */
/* Is the line starting at `ls` inside a heredoc body, its terminator
   included? The lines are read from the start of `buf` with
   sp_require_find's view of strings, comments and heredoc openers. */
static int sp_line_in_heredoc(const char *buf, size_t ls) {
  char term[128];
  term[0] = 0;
  const char *line = buf, *stop = buf + ls;
  while (*line && line <= stop) {
    const char *line_end = strchr(line, '\n');
    if (!line_end) line_end = line + strlen(line);
    if (term[0]) {
      if (line == stop) return 1;
      const char *t = line;
      while (t < line_end && (*t == ' ' || *t == '\t')) t++;
      size_t tl = strlen(term);
      if ((size_t)(line_end - t) == tl && strncmp(t, term, tl) == 0) term[0] = 0;
    }
    else {
      if (line == stop) return 0;
      for (const char *p = line; p < line_end; p++) {
        if (*p == '#') break;
        if (*p == '"' || *p == '\'') {
          char q = *p++;
          while (p < line_end && *p != q) { if (*p == '\\') p++; p++; }
          if (p >= line_end) break;
          continue;
        }
        if (p[0] == '<' && p + 1 < line_end && p[1] == '<' && (p == line || !sp_req_ident_char(p[-1]))) {
          const char *t = p + 2;
          if (t < line_end && (*t == '~' || *t == '-')) t++;
          char tq = 0;
          if (t < line_end && (*t == '"' || *t == '\'')) tq = *t++;
          const char *ts = t;
          while (t < line_end && sp_req_ident_char(*t)) t++;
          if (t > ts && (!tq || (t < line_end && *t == tq))) {
            size_t tl = (size_t)(t - ts);
            if (tl < sizeof term) { memcpy(term, ts, tl); term[tl] = 0; }
          }
        }
      }
    }
    if (*line_end != '\n') break;
    line = line_end + 1;
  }
  return 0;
}
static size_t sp_toplevel_stmt_start(const char *buf, size_t pos) {
  size_t ls = pos;
  while (ls > 0 && buf[ls - 1] != '\n') ls--;
  for (;;) {
    const char *l = buf + ls;
    int margin = *l && *l != ' ' && *l != '\t' && *l != '\n' && *l != '\r' && *l != '#';
    static const char *const cont[] = { "end", "else", "elsif", "when", "in ", "rescue", "ensure",
                                        "then", "do", "}", ")", "]", "=end", "=begin", NULL };
    if (margin) {
      int is_cont = 0;
      for (int i = 0; cont[i]; i++) {
        size_t n = strlen(cont[i]);
        if (strncmp(l, cont[i], n) == 0 && (cont[i][n - 1] == ' ' || !sp_req_ident_char(l[n]))) { is_cont = 1; break; }
      }
      /* (a heredoc's body line or terminator at the margin is no
         statement: content spliced ahead of it went into the String) */
      if (!is_cont && !sp_line_in_heredoc(buf, ls)) return ls;
    }
    if (ls == 0) return 0;
    ls--;                                   /* onto the previous line's newline */
    while (ls > 0 && buf[ls - 1] != '\n') ls--;
  }
}

/* A require standing alone on its line whose enclosing lines, out to the
   margin, are all control flow -- `if`/`elsif`/`else`/`unless`/`case`/`when`/
   `begin`/`rescue`/`ensure`/`while`/`until` -- may be inlined where it stands:
   a class or module definition is legal there, and the file then loads only
   when that branch runs (ffi-yajl requires its C-extension flavour or its FFI
   one on ENV["FORCE_FFI_YAJL"], inside begin/rescue LoadError). */
/* Inside a file that was itself inlined under a condition (between the
   markers the conditional splice writes around it)? */
static int sp_in_cond_region(const char *buf, const char *pos) {
  int depth = 0;
  for (const char *p = buf; p && p < pos; ) {
    const char *a = strstr(p, "#<SPINEL_COND");
    const char *z = strstr(p, "#</SPINEL_COND");
    const char *n = a && (!z || a < z) ? a : z;
    if (!n || n >= pos) break;
    depth += (n == a) ? 1 : -1;
    p = n + 1;
  }
  return depth > 0;
}

static int sp_req_in_control_only(const char *buf, const char *kw) {
  const char *ls = kw;
  while (ls > buf && ls[-1] != '\n') ls--;
  for (const char *q = ls; q < kw; q++) if (*q != ' ' && *q != '\t') return 0;
  size_t indent = (size_t)(kw - ls);
  if (indent == 0) return 0;
  static const char *const CTL[] = { "if", "elsif", "else", "unless", "case", "when", "begin",
                                     "rescue", "ensure", "while", "until", NULL };
  const char *l = ls;
  while (l > buf) {
    const char *pe = l - 1;              /* the previous line's newline */
    const char *pl = pe;
    while (pl > buf && pl[-1] != '\n') pl--;
    l = pl;
    const char *t = pl;
    while (t < pe && (*t == ' ' || *t == '\t')) t++;
    if (t == pe || *t == '#') continue;  /* blank or comment */
    size_t ind = (size_t)(t - pl);
    if (ind >= indent) continue;
    int ctl = 0;
    for (int i = 0; CTL[i]; i++) {
      size_t n = strlen(CTL[i]);
      if (strncmp(t, CTL[i], n) == 0 && !sp_req_ident_char(t[n])) { ctl = 1; break; }
    }
    if (!ctl) return 0;
    indent = ind;
    if (indent == 0) return 1;
  }
  return 0;
}

static void sp_req_hoist_splice(char **result, unsigned char **fsl, size_t *fsl_n,
                                const SpReqHit *h, const char *content,
                                unsigned char *cfsl, size_t cfsl_n,
                                const char *value) {
  size_t kw_off = (size_t)(h->kw - *result), end_off = (size_t)(h->expr_end - *result);
  size_t rlen = strlen(*result), clen = strlen(content), vlen = strlen(value);
  size_t ins = sp_toplevel_stmt_start(*result, kw_off);
  size_t ins_line = 0;
  for (size_t i = 0; i < ins; i++) if ((*result)[i] == '\n') ins_line++;
  sp_fsl_splice(fsl, fsl_n, ins_line, 0, cfsl, cfsl_n);
  /* The caller's statement survives this splice. Only its included file's
     outer marker changes: nested requires retain their own accounting. */
  size_t skip = strncmp(content, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) == 0
              ? strlen(SP_PUSH_PREFIX) : 0;
  size_t add = skip ? strlen(SP_INSERT_PREFIX) : 0;
  char *nr = malloc(rlen + clen + vlen + add - skip + 2);
  size_t o = 0;
  memcpy(nr + o, *result, ins);                          o += ins;
  if (add) { memcpy(nr + o, SP_INSERT_PREFIX, add); o += add; }
  content += skip; clen -= skip;
  memcpy(nr + o, content, clen);                         o += clen;
  if (clen > 0 && content[clen - 1] != '\n') nr[o++] = '\n';
  memcpy(nr + o, *result + ins, kw_off - ins);           o += kw_off - ins;
  memcpy(nr + o, value, vlen);                           o += vlen;
  memcpy(nr + o, *result + end_off, rlen - end_off + 1);
  free(*result);
  *result = nr;
}

static int sp_req_if_modifier(const char *t) {
  int n = (int)strspn(t, " \t"), k = strncmp(t + n, "if", 2) == 0 ? 2 : strncmp(t + n, "unless", 6) == 0 ? 6 : 0;
  if (!k || sp_req_ident_char(t[n + k])) return 0;
  int c = n += k, e = 0;
  for (; t[n] && t[n] != '\n' && t[n] != '\r' && (e || t[n] != ';'); n++) {
    if (e) continue;
    if (t[n] == '#') e = n;
    else if (t[n] == '"' || t[n] == '\'') {
      char q = t[n];
      while (t[++n] && t[n] != q && t[n] != '\n') if (t[n] == '\\' && t[n + 1]) n++;
      if (t[n] != q) return 0;
    }
  }
  for (e = e ? e : n; e > c && (t[e - 1] == ' ' || t[e - 1] == '\t'); e--) {}
  if (e == c || t[n] == ';' || strchr("&|,(\\+-*/.<>=:", t[e - 1])) return 0;
  return (e - c > 3 && (!strncmp(t + e - 4, " and", 4) || !strncmp(t + e - 4, " not", 4))) ||
         (e - c > 2 && !strncmp(t + e - 3, " or", 3)) ? 0 : n;
}

static char *sp_req_cond_wrap(char *content, unsigned char **cfsl, size_t *cfsl_n, const char *head, int hl) {
  size_t wcl = strlen(content), skip = hl && strncmp(content, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) == 0 ? strlen(SP_PUSH_PREFIX) : 0;
  char *w = malloc(wcl + hl + 48);
  if (!w) return content;
  sprintf(w, "#<SPINEL_COND>\n%.*s%s%s%s%s%s#</SPINEL_COND>\n", hl, head, hl ? "\n" : "", skip ? SP_INSERT_PREFIX : "",
          content + skip, (wcl && content[wcl - 1] != '\n') ? "\n" : "", hl ? "end " : "");
  free(content);
  unsigned char z[2] = { 0, 0 };
  sp_fsl_splice(cfsl, cfsl_n, 0, 0, z, hl ? 2 : 1);
  sp_fsl_splice(cfsl, cfsl_n, *cfsl_n, 0, z, 1);
  return w;
}

/* ---- computed requires a file's own layout decides ----
 *
 * `require File.join(Archive::LIBPATH, "ffi-libarchive", "archive")`,
 * `require ZMQ.libpath(["ffi-rzmq", file])` inside `%w(util context).each do
 * |file|`: the name is computed, but from literals that spell a path under the
 * requiring file's own directory (the Mr Bones gem template, among others).
 * Such a require becomes `require_relative` of the joined literals when that
 * file exists; a `%w(...).each` loop around it is unrolled first. Anything
 * that does not resolve to a file is left for the ordinary pass. */
static int sp_path_exists(const char *p) { FILE *f = fopen(p, "r"); if (f) { fclose(f); return 1; } return 0; }

/* The string literals of `arg` (up to `end`) joined by '/', with `var`
   standing in as `val` wherever it appears as a bare word. */
static int sp_join_literals(const char *arg, const char *end, const char *var, const char *val,
                            char *out, size_t cap) {
  size_t o = 0; int n = 0;
  out[0] = 0;
  for (const char *p = arg; p < end; p++) {
    const char *piece = NULL; size_t plen = 0;
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      const char *st = p;
      while (p < end && *p != q) { if (*p == '\\') p++; p++; }
      if (p >= end) return 0;
      piece = st; plen = (size_t)(p - st);
      if (memchr(piece, '#', plen)) return 0;   /* interpolation: not a literal */
    }
    else if (var && sp_req_ident_char(*p) && (p == arg || !sp_req_ident_char(p[-1]) ) &&
             strncmp(p, var, strlen(var)) == 0 && !sp_req_ident_char(p[strlen(var)])) {
      piece = val; plen = strlen(val);
      p += strlen(var) - 1;
    }
    else continue;
    if (plen == 0) continue;
    if (plen == 1 && piece[0] == '.') continue;          /* a '.' segment */
    if (plen == 1 && piece[0] == '/') continue;
    if (o + plen + 2 >= cap) return 0;
    if (n++) out[o++] = '/';
    memcpy(out + o, piece, plen); o += plen;
    out[o] = 0;
  }
  /* drop a trailing .rb: require_relative adds it back */
  if (o > 3 && strcmp(out + o - 3, ".rb") == 0) out[o - 3] = 0;
  return n > 0;
}

/* `require <computed>` on one line -> `require_relative "<joined>"` when the
   joined literals name a file beside this one. The call must be the whole
   statement (only a comment may follow it: a modifier or a `;` would be
   lost), and a path based on __FILE__ is left alone -- its literals are
   relative to the file, not to its directory. */
static int sp_computed_require(const char *line, const char *eol, const char *dir,
                               const char *var, const char *val, char *out, size_t cap) {
  const char *p = line;
  while (p < eol && (*p == ' ' || *p == '\t')) p++;
  if (strncmp(p, "require", 7) != 0 || sp_req_ident_char(p[7])) return 0;
  const char *a = p + 7;
  int paren = 0;
  while (a < eol && (*a == ' ' || *a == '\t')) a++;
  if (a < eol && *a == '(') { paren = 1; a++; }
  while (a < eol && (*a == ' ' || *a == '\t')) a++;
  if (a >= eol || *a == '"' || *a == '\'') return 0;        /* a plain literal: the ordinary pass */
  /* the argument's extent: to the matching `)`, or to the line's end */
  const char *ae = NULL;
  int depth = paren;
  char q = 0;
  for (const char *c = a; c < eol; c++) {
    if (q) { if (*c == '\\') c++; else if (*c == q) q = 0; continue; }
    if (*c == '"' || *c == '\'') { q = *c; continue; }
    if (*c == '#') { if (paren) return 0; ae = c; break; }
    if (*c == ';') return 0;
    if (*c == '(' || *c == '[') depth++;
    else if (*c == ')' || *c == ']') {
      if (--depth == 0 && paren) { ae = c; break; }
    }
    /* a modifier on the call: `if` / `unless` / `rescue` / `while` / `until` */
    if (depth == (paren ? 1 : 0) && (c == a || c[-1] == ' ' || c[-1] == ')') &&
        ((strncmp(c, "if", 2) == 0 && !sp_req_ident_char(c[2])) ||
         (strncmp(c, "unless", 6) == 0 && !sp_req_ident_char(c[6])) ||
         (strncmp(c, "rescue", 6) == 0 && !sp_req_ident_char(c[6])) ||
         (strncmp(c, "while", 5) == 0 && !sp_req_ident_char(c[5])) ||
         (strncmp(c, "until", 5) == 0 && !sp_req_ident_char(c[5]))))
      return 0;
  }
  if (q) return 0;
  if (!ae) { if (paren) return 0; ae = eol; }
  if (paren) {                                   /* only a comment after `)` */
    const char *t = ae + 1;
    while (t < eol && (*t == ' ' || *t == '\t' || *t == '\r')) t++;
    if (t < eol && *t != '#') return 0;
  }
  for (const char *c = a; c + 8 <= ae; c++) if (strncmp(c, "__FILE__", 8) == 0) return 0;
  char joined[512];
  if (!sp_join_literals(a, ae, var, val, joined, sizeof joined)) return 0;
  char full[1100];
  snprintf(full, sizeof full, "%s/%s.rb", dir, joined);
  if (!sp_path_exists(full)) return 0;
  snprintf(out, cap, "%.*srequire_relative \"%s\"\n", (int)(p - line), line, joined);
  return 1;
}

static char *sp_rewrite_computed_requires(const char *source, const char *dir) {
  size_t slen = strlen(source);
  size_t cap = slen * 2 + 4096;
  char *out = malloc(cap);
  if (!out) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t o = 0;
  const char *p = source;
  while (*p) {
    const char *eol = strchr(p, '\n');
    const char *next = eol ? eol + 1 : p + strlen(p);
    if (!eol) eol = p + strlen(p);
    char repl[1400];
    /* `%w(a b).each do |v|` / `require ...v...` / `end`, or the brace form
       on one line */
    const char *w = p;
    while (w < eol && (*w == ' ' || *w == '\t')) w++;
    if (strncmp(w, "%w(", 3) == 0 || strncmp(w, "%w[", 3) == 0 || strncmp(w, "%w{", 3) == 0) {
      char close = w[2] == '(' ? ')' : (w[2] == '[' ? ']' : '}');
      const char *le = memchr(w + 3, close, (size_t)(eol - w - 3));
      const char *each = le ? strstr(le, ".each") : NULL;
      if (le && each && each < eol) {
        const char *bar = memchr(each, '|', (size_t)(eol - each));
        const char *bar2 = bar ? memchr(bar + 1, '|', (size_t)(eol - bar - 1)) : NULL;
        if (bar && bar2) {
          char var[64];
          const char *vs = bar + 1; while (*vs == ' ') vs++;
          size_t vl = 0; while (vs + vl < bar2 && sp_req_ident_char(vs[vl])) vl++;
          if (vl > 0 && vl < sizeof var) {
            memcpy(var, vs, vl); var[vl] = 0;
            int brace = memchr(each, '{', (size_t)(bar - each)) != NULL;
            const char *body = brace ? bar2 + 1 : next;
            const char *body_eol = brace ? eol : strchr(next, '\n');
            const char *after = NULL;
            if (!brace && body_eol) {
              const char *l3 = body_eol + 1;
              const char *l3e = strchr(l3, '\n'); if (!l3e) l3e = l3 + strlen(l3);
              const char *t = l3; while (t < l3e && (*t == ' ' || *t == '\t')) t++;
              if (strncmp(t, "end", 3) == 0 && !sp_req_ident_char(t[3])) after = *l3e ? l3e + 1 : l3e;
            }
            if (brace) {
              const char *cb = strrchr(body, '}');
              if (cb && cb < eol) { body_eol = cb; after = next; }
            }
            if (after && body_eol) {
              /* every word of the list must resolve, or nothing is unrolled */
              int old_lines = 0; for (const char *q = p; q < after; q++) if (*q == '\n') old_lines++;
              if (old_lines < 1) old_lines = 1;
              char unrolled[8192]; size_t uo = 0; int ok = 1, nw = 0;
              const char *e = w + 3;
              while (ok && e < le) {
                while (e < le && (*e == ' ' || *e == '\t' || *e == '\n')) e++;
                const char *es = e;
                while (e < le && *e != ' ' && *e != '\t' && *e != '\n') e++;
                if (e == es) break;
                char val[128]; snprintf(val, sizeof val, "%.*s", (int)(e - es), es);
                char one[1400];
                if (!sp_computed_require(body, body_eol, dir, var, val, one, sizeof one)) { ok = 0; break; }
                const char *ind = one; while (*ind == ' ' || *ind == '\t') ind++;
                size_t il = strlen(ind);
                if (il && ind[il - 1] == '\n') il--;
                /* one require per line while the loop's lines last, then the
                   rest joined by `; ` on its last line */
                int own_line = nw < old_lines;
                size_t lead = own_line ? (size_t)(w - p) + (nw ? 1 : 0) : 2;
                if (uo + lead + il + 2 >= sizeof unrolled) { ok = 0; break; }
                if (own_line) {
                  if (nw) unrolled[uo++] = '\n';
                  memcpy(unrolled + uo, p, (size_t)(w - p)); uo += (size_t)(w - p);
                }
                else { unrolled[uo++] = ';'; unrolled[uo++] = ' '; }
                memcpy(unrolled + uo, ind, il); uo += il;
                nw++;
              }
              if (ok && uo > 0) {
                unrolled[uo++] = '\n';
                /* the replaced lines keep their count: pad with blank lines so
                   line numbers after the loop do not move */
                int new_lines = nw < old_lines ? nw : old_lines;
                if (o + uo + (size_t)old_lines + 64 >= cap) { cap = cap * 2 + uo + (size_t)old_lines; out = realloc(out, cap); }
                memcpy(out + o, unrolled, uo); o += uo;
                for (; new_lines < old_lines; new_lines++) out[o++] = '\n';
                p = after;
                continue;
              }
            }
          }
        }
      }
    }
    if (sp_computed_require(p, eol, dir, NULL, NULL, repl, sizeof repl)) {
      size_t rl = strlen(repl);
      if (o + rl + 64 >= cap) { cap = cap * 2 + rl; out = realloc(out, cap); }
      memcpy(out + o, repl, rl); o += rl;
      p = next;
      continue;
    }
    size_t ll = (size_t)(next - p);
    if (o + ll + 64 >= cap) { cap = cap * 2 + ll; out = realloc(out, cap); }
    memcpy(out + o, p, ll); o += ll;
    p = next;
  }
  out[o] = 0;
  return out;
}

/* `autoload :Name, "path"` with a literal path: the whole program is
   compiled, so the file is loaded eagerly. In the entry file the call becomes
   the require in place; in a required file it answers nil there, and the
   requires go at the end of the file -- after the module body the autoload
   sits in, which the loaded file usually reopens. A path that starts with
   `#{__dir__}/` is literal enough: it is a require_relative, and so is
   `File.expand_path('path', __dir__)`. In a required file the name may be a
   local (`%w[A B].each { |n| autoload n, ... }`): loaded eagerly, the file
   is the same whatever name it was asked for. A receiver form
   (`Mod.autoload`) or any other computed path is left as it was. */
static int sp_autoload_is_main = 0;
/* the autoload's file beside the one naming it (lib/foo.rb autoloading
   "foo/bar" is lib/foo/bar.rb, the gem layout): reached without a load path */
static int sp_autoload_beside(const char *dir, const char *path, size_t plen) {
  char fp[1400];
  snprintf(fp, sizeof fp, "%s/%.*s%s", dir, (int)plen, path,
           (plen >= 3 && strncmp(path + plen - 3, ".rb", 3) == 0) ? "" : ".rb");
  FILE *f = fopen(fp, "r");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static char *sp_rewrite_autoloads(const char *source, const char *dir) {
  size_t slen = strlen(source);
  /* a call becomes at most `require_relative "path"` (plus a newline in the
     tail), under twice the length of the shortest `autoload :X,"p"` */
  char *out = malloc(slen * 2 + 64);
  char *tail = malloc(slen * 2 + 64);
  if (!out || !tail) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t o = 0, t = 0;
  const char *p = source;
  int in_comment = 0;
  char quote = 0;
  while (*p) {
    char ch = *p;
    if (ch == '\n') { in_comment = 0; quote = 0; out[o++] = *p++; continue; }
    if (in_comment) { out[o++] = *p++; continue; }
    if (quote) {
      if (ch == '\\' && p[1]) { out[o++] = *p++; out[o++] = *p++; continue; }
      if (ch == quote) quote = 0;
      out[o++] = *p++;
      continue;
    }
    if (ch == '#') { in_comment = 1; out[o++] = *p++; continue; }
    if (ch == '"' || ch == '\'') { quote = ch; out[o++] = *p++; continue; }
    if (strncmp(p, "autoload", 8) == 0 &&
        (p == source || (!sp_req_ident_char(p[-1]) && p[-1] != ':' && p[-1] != '@' && p[-1] != '$' &&
                         p[-1] != '.')) &&
        !sp_req_ident_char(p[8]) && p[8] != '?') {
      const char *q = p + 8;
      int paren = 0;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '(') { paren = 1; q++; while (*q == ' ' || *q == '\t') q++; }
      /* the name: a Symbol, or -- in a required file, whose requires go
         at its end -- a local naming it (`%w[A B].each { |n| autoload n,
         ... }`): the file is loaded once whatever names it holds */
      int named = 0;
      if (*q == ':') { q++; named = 1; }
      else if (!sp_autoload_is_main && ((*q >= 'a' && *q <= 'z') || *q == '_')) named = 1;
      if (named) {
        const char *ns = q;
        while (sp_req_ident_char(*q)) q++;
        if (q > ns) {
          while (*q == ' ' || *q == '\t') q++;
          if (*q == ',') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            /* `File.expand_path('path', __dir__)` names the file beside this
               one, as `"#{__dir__}/path"` does */
            int xp = 0;
            if (strncmp(q, "File.expand_path(", 17) == 0) {
              const char *xq = q + 17;
              while (*xq == ' ' || *xq == '\t') xq++;
              if (*xq == '"' || *xq == '\'') {
                char xc = *xq;
                const char *xe = xq + 1;
                while (*xe && *xe != xc && *xe != '\n' && *xe != '#' && *xe != '\\') xe++;
                if (*xe == xc) {
                  const char *xt = xe + 1;
                  while (*xt == ' ' || *xt == '\t') xt++;
                  if (*xt == ',') {
                    xt++;
                    while (*xt == ' ' || *xt == '\t') xt++;
                    if (strncmp(xt, "__dir__", 7) == 0 && !sp_req_ident_char(xt[7])) {
                      xt += 7;
                      while (*xt == ' ' || *xt == '\t') xt++;
                      if (*xt == ')') { xp = 1; q = xq; }
                    }
                  }
                }
              }
            }
            if (*q == '"' || *q == '\'') {
              char qq = *q++;
              /* `"#{__dir__}/path"` (the gem layout: lib/rdoc.rb autoloading
                 "#{__dir__}/rdoc/x"): the one interpolation a path can carry
                 and still name a file known at compile time. It is the
                 requiring file's own directory, so the call is
                 `require_relative "path"`. Any other `#{` is a computed path
                 and leaves the call as it was. */
              int dir_rel = xp;
              if (!xp && qq == '"' && strncmp(q, "#{__dir__}/", 11) == 0) { q += 11; dir_rel = 1; }
              const char *fs = q;
              while (*q && *q != qq && *q != '\n' && *q != '#' ) q++;
              if (*q == qq) {
                size_t flen = (size_t)(q - fs);
                q++;
                /* past the expand_path call's `, __dir__)` */
                if (xp) { while (*q && *q != ')') q++; if (*q == ')') q++; }
                while (*q == ' ' || *q == '\t') q++;
                if (!paren || *q == ')') {
                  if (paren) q++;
                  const char *kw = (dir_rel || sp_autoload_beside(dir, fs, flen)) ? "require_relative" : "require";
                  if (sp_autoload_is_main) {
                    o += (size_t)sprintf(out + o, "%s \"%.*s\"", kw, (int)flen, fs);
                  }
                  else {
                    out[o++] = 'n'; out[o++] = 'i'; out[o++] = 'l';
                    t += (size_t)sprintf(tail + t, "%s \"%.*s\"\n", kw, (int)flen, fs);
                  }
                  p = q;
                  continue;
                }
              }
            }
          }
        }
      }
    }
    out[o++] = *p++;
  }
  if (t > 0) {
    if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
    memcpy(out + o, tail, t);
    o += t;
  }
  out[o] = 0;
  free(tail);
  return out;
}

/* Dependency resolution precedes analysis, so even `if false; require ...`
   would otherwise read the file (or reject a missing one). Only erase literal
   require calls in provably unreachable branches: keep the surrounding code
   for Ruby's lexical locals, and preserve every newline for the source map. */
typedef struct {
  pm_parser_t *parser;
  const char *source;
  char *result;
  int dead;
  int engine;   /* fold RUBY_ENGINE comparisons (the file does not assign the constant) */
} SpDeadRequire;

static int sp_pm_name_is(const pm_parser_t *parser, pm_constant_id_t id, const char *word) {
  const pm_constant_t *name = pm_constant_pool_id_to_constant(&parser->constant_pool, id);
  size_t n = strlen(word);
  return name->length == n && memcmp(name->start, word, n) == 0;
}

/* The truth of a predicate the splice can settle without the analyzer:
   1 / 0 for a literal or a settled RUBY_ENGINE comparison, -1 otherwise.
   RUBY_ENGINE is "spinel" in every program spinel compiles, so
   `RUBY_ENGINE == "x"` (either operand order), its `!=` and a `!` around
   either are constants too. The analyzer drops the branch such a check rules
   out before anything in it is compiled (desugar_engine_branches), but the
   splice runs first and used to fetch -- or warn about -- a file no one would
   use: the harness a benchmark keeps for CRuby, a JRuby shim. */
static int sp_require_truth(const pm_parser_t *parser, const pm_node_t *node, int engine) {
  if (!node) return -1;
  switch (PM_NODE_TYPE(node)) {
    case PM_TRUE_NODE: return 1;
    case PM_FALSE_NODE: case PM_NIL_NODE: return 0;
    case PM_PARENTHESES_NODE:
      return sp_require_truth(parser, ((const pm_parentheses_node_t *)node)->body, engine);
    case PM_STATEMENTS_NODE: {
      const pm_node_list_t *body = &((const pm_statements_node_t *)node)->body;
      return body->size == 1 ? sp_require_truth(parser, body->nodes[0], engine) : -1;
    }
    case PM_CALL_NODE: {
      const pm_call_node_t *call = (const pm_call_node_t *)node;
      if (call->block || !call->receiver) return -1;
      if (sp_pm_name_is(parser, call->name, "!") && !call->arguments) {
        int truth = sp_require_truth(parser, call->receiver, engine);
        return truth < 0 ? -1 : !truth;
      }
      int eq = sp_pm_name_is(parser, call->name, "==");
      if (!engine || (!eq && !sp_pm_name_is(parser, call->name, "!="))) return -1;
      if (!call->arguments || call->arguments->arguments.size != 1) return -1;
      const pm_node_t *a = call->receiver, *b = call->arguments->arguments.nodes[0];
      const pm_node_t *cst, *lit;
      if (PM_NODE_TYPE(a) == PM_CONSTANT_READ_NODE && PM_NODE_TYPE(b) == PM_STRING_NODE) { cst = a; lit = b; }
      else if (PM_NODE_TYPE(b) == PM_CONSTANT_READ_NODE && PM_NODE_TYPE(a) == PM_STRING_NODE) { cst = b; lit = a; }
      else return -1;
      if (!sp_pm_name_is(parser, ((const pm_constant_read_node_t *)cst)->name, "RUBY_ENGINE")) return -1;
      const pm_string_t *s = &((const pm_string_node_t *)lit)->unescaped;
      int same = pm_string_length(s) == 6 && memcmp(pm_string_source(s), "spinel", 6) == 0;
      return eq ? same : !same;
    }
    default: return -1;
  }
}

/* Does the source assign RUBY_ENGINE itself (a shim's `RUBY_ENGINE = "jruby"`)?
   Then a comparison reads that constant, not the engine's, and is left alone --
   the same rule desugar_engine_branches applies. */
static int sp_source_writes_engine(const char *source) {
  for (const char *p = source; (p = strstr(p, "RUBY_ENGINE")); p += 11) {
    const char *q = p + 11;
    while (*q == ' ' || *q == '\t') q++;
    if (*q == '=' && q[1] != '=' && q[1] != '~') return 1;
    if ((q[0] == '|' && q[1] == '|' && q[2] == '=') || (q[0] == '&' && q[1] == '&' && q[2] == '=')) return 1;
  }
  return 0;
}

/* `Kernel.require "x"` (`::Kernel.` too) is the bare require: the textual
   resolver has treated it so since 0c0f61fff, so a dead branch must drop it
   exactly as it drops a receiver-less `require`, or --require-gate refuses
   a call that never runs. */
static int sp_call_receiver_is_kernel(const pm_parser_t *parser, const pm_node_t *receiver) {
  if (!receiver) return 0;
  if (PM_NODE_TYPE(receiver) == PM_CONSTANT_READ_NODE)
    return sp_pm_name_is(parser, ((const pm_constant_read_node_t *)receiver)->name, "Kernel");
  if (PM_NODE_TYPE(receiver) == PM_CONSTANT_PATH_NODE) {
    const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)receiver;
    return !cp->parent && sp_pm_name_is(parser, cp->name, "Kernel");
  }
  return 0;
}

/* Blank a statement in a dead branch to `(nil)`, keeping every newline so
   later line numbers hold (and a multiline call stays grouped, including
   before a modifier). */
static void sp_dead_blank(SpDeadRequire *ctx, const pm_node_t *node) {
  size_t start = (size_t)(node->location.start - (const uint8_t *)ctx->source);
  size_t end = (size_t)(node->location.end - (const uint8_t *)ctx->source);
  for (size_t i = start; i < end; i++)
    if (ctx->result[i] != '\n' && ctx->result[i] != '\r') ctx->result[i] = ' ';
  memcpy(ctx->result + start, "(nil", 4);
  ctx->result[end - 1] = ')';
}

static bool sp_skip_dead_require(const pm_node_t *node, void *data) {
  SpDeadRequire *ctx = data;
  if (!ctx->dead && (PM_NODE_TYPE(node) == PM_IF_NODE || PM_NODE_TYPE(node) == PM_UNLESS_NODE)) {
    const pm_node_t *predicate, *body, *other;
    int unless = PM_NODE_TYPE(node) == PM_UNLESS_NODE;
    if (unless) {
      const pm_unless_node_t *n = (const pm_unless_node_t *)node;
      predicate = n->predicate; body = (const pm_node_t *)n->statements;
      other = (const pm_node_t *)n->else_clause;
    }
    else {
      const pm_if_node_t *n = (const pm_if_node_t *)node;
      predicate = n->predicate; body = (const pm_node_t *)n->statements;
      other = n->subsequent;
    }
    int truth = sp_require_truth(ctx->parser, predicate, ctx->engine);
    if (truth >= 0) {
      SpDeadRequire branch = *ctx;
      branch.dead = truth == unless;
      if (body) pm_visit_node(body, sp_skip_dead_require, &branch);
      branch.dead = !branch.dead;
      if (other) pm_visit_node(other, sp_skip_dead_require, &branch);
      return false;
    }
  }
  if (ctx->dead && PM_NODE_TYPE(node) == PM_CALL_NODE) {
    const pm_call_node_t *call = (const pm_call_node_t *)node;
    const pm_constant_t *name = pm_constant_pool_id_to_constant(&ctx->parser->constant_pool, call->name);
    if ((!call->receiver || sp_call_receiver_is_kernel(ctx->parser, call->receiver)) &&
        !call->block && call->arguments && call->arguments->arguments.size == 1 &&
        ((name->length == 7 && memcmp(name->start, "require", 7) == 0) ||
         (name->length == 16 && memcmp(name->start, "require_relative", 16) == 0))) {
      const pm_node_t *arg = call->arguments->arguments.nodes[0];
      if (PM_NODE_TYPE(arg) == PM_STRING_NODE) {
        const pm_string_node_t *str = (const pm_string_node_t *)arg;
        /* Heredoc bodies can lie outside the call's span. Match the quoted
           literals the textual require resolver accepts. */
        if (str->opening_loc.start && (*str->opening_loc.start == '\'' || *str->opening_loc.start == '"')) {
          sp_dead_blank(ctx, node);
          return false;
        }
      }
    }
    /* The `$LOAD_PATH << dir` that goes with a dead branch's require goes
       with it: the branch is dropped as the require is. */
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_GLOBAL_VARIABLE_READ_NODE &&
        call->arguments && call->arguments->arguments.size == 1 && !call->block) {
      const pm_global_variable_read_node_t *gv = (const pm_global_variable_read_node_t *)call->receiver;
      if ((sp_pm_name_is(ctx->parser, gv->name, "$LOAD_PATH") || sp_pm_name_is(ctx->parser, gv->name, "$:")) &&
          (sp_pm_name_is(ctx->parser, call->name, "<<") || sp_pm_name_is(ctx->parser, call->name, "unshift") ||
           sp_pm_name_is(ctx->parser, call->name, "push") || sp_pm_name_is(ctx->parser, call->name, "append") ||
           sp_pm_name_is(ctx->parser, call->name, "prepend"))) {
        sp_dead_blank(ctx, node);
        return false;
      }
    }
  }
  return true;
}

/* The file's code (its first `code_len` bytes: sp_src_file_code_len) with
   its dead requires blanked, read from the file's own parse. Only the code
   is read: a `RUBY_ENGINE = ...` in the data section after `__END__` is not
   an assignment, and the tree's nodes all lie before it, at the same
   offsets in the copy as in the file. */
static char *sp_rewrite_dead_requires(SpSrcFile *f, size_t code_len) {
  const char *source = f->text;
  char *result = malloc(code_len + 1);
  if (!result) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(result, source, code_len);
  result[code_len] = '\0';
  if (!strstr(result, "require")) return result;
  if (f->parser.error_list.size == 0) {
    SpDeadRequire ctx = { &f->parser, source, result, 0, !sp_source_writes_engine(result) };
    pm_visit_node(f->root, sp_skip_dead_require, &ctx);
  }
  return result;
}

/* g_require_unread: may the program load a file this compiler did not read?
   Such a file can define or reopen anything, so a pass that asks what the
   program gives a class has to take the answer as unknown where this is set.
   It stays clear only where the text proves there is no such file, read as
   written (ahead of every rewrite below) in each file handed to
   resolve_requires, up to the file's `__END__` line:
     - none of $LOAD_PATH, $:, $-I, $LOADED_FEATURES, $" is written, so a
       name is looked up where this compiler looks;
     - nothing named load or autoload is written;
     - every require and require_relative written was resolved: a file read,
       one read before, or a feature the runtime provides. One left as a
       raise or a warning, blanked as unreachable, or past where the scan
       gave up counts as written and not resolved; one a rewrite made sets
       the flag itself.
   Text the class macros write (sp_expand_class_macros) is read once they
   have, for the words that set the flag: the parts a macro joins need not
   spell one. The flag is final when the parser returns.
   The words are counted, not parsed, and no character of the code is passed
   over: one in a string or in a comment sets the flag, and all that gives up
   is an optimisation. Declared extern in compiler.h. */
int g_require_unread = 0;
/* The features this program was compiled with, as each require and
   require_relative names it: a require whose path is known only at run
   time answers for these as already loaded, and raises LoadError for any
   other (sp_require_computed). */
const char **g_required_features = NULL;
int g_nrequired_features = 0;
static void sp_note_required_feature(const char *name) {
  if (!name || !*name) return;
  for (int i = 0; i < g_nrequired_features; i++)
    if (strcmp(g_required_features[i], name) == 0) return;
  const char **nf = realloc(g_required_features, sizeof(char *) * (size_t)(g_nrequired_features + 1));
  if (!nf) return;
  g_required_features = nf;
  g_required_features[g_nrequired_features++] = strdup(name);
}
static int sp_req_written = 0, sp_req_resolved = 0;

static int sp_req_word_at(const char *p, const char *w, size_t n) {
  return strncmp(p, w, n) == 0 && !sp_req_ident_char(p[n]);
}

static void sp_require_note_text(const char *text, size_t len) {
  for (const char *p = text; p < text + len; p++) {
    if (*p == '$') {
      if (p[1] == ':' || p[1] == '"' || strncmp(p + 1, "-I", 2) == 0 || strncmp(p + 1, "LOAD_PATH", 9) == 0 ||
          strncmp(p + 1, "LOADED_FEATURES", 15) == 0) g_require_unread = 1;
      continue;
    }
    if ((*p != 'r' && *p != 'l' && *p != 'a') || (p > text && sp_req_ident_char(p[-1]))) continue;
    if (sp_req_word_at(p, "require", 7) || sp_req_word_at(p, "require_relative", 16)) sp_req_written++;
    else if (sp_req_word_at(p, "load", 4) || sp_req_word_at(p, "autoload", 8)) g_require_unread = 1;
  }
}

/* ---- a require that runs later than the program start ----
 *
 * A require inside a method or a block (minitest's `-W` option block requires
 * minitest/error_on_warning) is hoisted: the file is spliced ahead of the
 * top-level statement it sits in, so its classes and methods exist for the
 * whole program. Its run-time statements would then run at startup, not when
 * the require does: `::Warning.singleton_class.prepend ErrorOnWarning` in that
 * file ran in every minitest binary. Such statements -- a call on a computed
 * receiver, or one that prints -- are wrapped in place as methods of their own
 * scope (`def self.__spinel_load_N_K; stmt; end`, top-level ones as private
 * methods of main), and the require's value becomes a call that runs them the
 * first time it is reached. Definitions and class macros stay where they are.
 * A statement that reads the file's locals, `self` or an instance variable at
 * the top level, or returns or yields, stays too: in a method it would mean
 * something else. Every edit stays on its statement's lines. */
typedef struct {
  const pm_parser_t *parser;
  const pm_constant_id_list_t *locals;
  int top, bad;
} SpDeferScan;

static bool sp_defer_scan(const pm_node_t *node, void *data) {
  SpDeferScan *s = data;
  pm_constant_id_t name = 0;
  switch (PM_NODE_TYPE(node)) {
    case PM_RETURN_NODE: case PM_YIELD_NODE: case PM_FORWARDING_SUPER_NODE: case PM_SUPER_NODE:
    case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE:
      s->bad = 1; return false;
    case PM_SELF_NODE: case PM_INSTANCE_VARIABLE_READ_NODE: case PM_INSTANCE_VARIABLE_WRITE_NODE:
    case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: case PM_INSTANCE_VARIABLE_OR_WRITE_NODE:
    case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: case PM_INSTANCE_VARIABLE_TARGET_NODE:
      if (s->top) { s->bad = 1; return false; }
      return true;
    case PM_LOCAL_VARIABLE_READ_NODE: name = ((const pm_local_variable_read_node_t *)node)->name; break;
    case PM_LOCAL_VARIABLE_WRITE_NODE: name = ((const pm_local_variable_write_node_t *)node)->name; break;
    case PM_LOCAL_VARIABLE_TARGET_NODE: name = ((const pm_local_variable_target_node_t *)node)->name; break;
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: name = ((const pm_local_variable_operator_write_node_t *)node)->name; break;
    case PM_LOCAL_VARIABLE_OR_WRITE_NODE: name = ((const pm_local_variable_or_write_node_t *)node)->name; break;
    case PM_LOCAL_VARIABLE_AND_WRITE_NODE: name = ((const pm_local_variable_and_write_node_t *)node)->name; break;
    default: return true;
  }
  if (pm_constant_id_list_includes((pm_constant_id_list_t *)s->locals, name)) { s->bad = 1; return false; }
  return true;
}

/* `Foo.include Bar` and the like: on a named class, a call that shapes it */
static int sp_defer_shape_call(const pm_parser_t *parser, const pm_call_node_t *call) {
  static const char *const shape[] = {
    "include", "extend", "prepend", "attr_reader", "attr_writer", "attr_accessor", "attr",
    "private", "public", "protected", "module_function", "private_constant", "public_constant",
    "define_method", "alias_method", "remove_method", "undef_method", "private_class_method",
    "public_class_method", "const_set", "class_eval", "module_eval", "class_exec", "module_exec",
    "instance_eval", "send", "__send__", "public_send", "freeze", NULL };
  for (int i = 0; shape[i]; i++) if (sp_pm_name_is(parser, call->name, shape[i])) return 1;
  return 0;
}

/* Does `stmt`, standing in a file's top level or a class body, only run code
   (as opposed to defining something the program's shape depends on)? */
static int sp_defer_stmt(const pm_parser_t *parser, const pm_node_t *stmt,
                         const pm_constant_id_list_t *locals, int top) {
  if (PM_NODE_TYPE(stmt) != PM_CALL_NODE) return 0;
  const pm_call_node_t *call = (const pm_call_node_t *)stmt;
  if (call->receiver) {
    int recv_t = PM_NODE_TYPE(call->receiver);
    if (recv_t == PM_SELF_NODE) return 0;
    if ((recv_t == PM_CONSTANT_READ_NODE || recv_t == PM_CONSTANT_PATH_NODE) &&
        sp_defer_shape_call(parser, call)) return 0;
  }
  else {
    static const char *const out[] = { "puts", "print", "p", "pp", "printf", "putc", "warn", NULL };
    int io = 0;
    for (int i = 0; out[i]; i++) if (sp_pm_name_is(parser, call->name, out[i])) io = 1;
    if (!io) return 0;
  }
  SpDeferScan s = { parser, locals, top, 0 };
  pm_visit_node(stmt, sp_defer_scan, &s);
  return !s.bad;
}

/* A `return` in the file's own top level (not in a method, block or lambda)
   ends the load early: the statements after it must not become methods
   whose definitions never run. */
static bool sp_defer_top_return(const pm_node_t *node, void *data) {
  int t = PM_NODE_TYPE(node);
  if (t == PM_DEF_NODE || t == PM_BLOCK_NODE || t == PM_LAMBDA_NODE) return false;
  if (t == PM_RETURN_NODE) { *(int *)data = 1; return false; }
  return true;
}

typedef struct { size_t at; char *text; } SpDeferEdit;
typedef struct {
  const pm_parser_t *parser;
  const char *source;
  int file_no, n;
  int out_of_order;   /* a statement left in place runs after a wrapped one */
  SpDeferEdit *edits; int edits_n, edits_cap;
  char *calls; size_t calls_len, calls_cap;
} SpDefer;

static void sp_defer_edit(SpDefer *d, size_t at, const char *text) {
  if (d->edits_n == d->edits_cap) {
    d->edits_cap = d->edits_cap ? d->edits_cap * 2 : 8;
    d->edits = realloc(d->edits, sizeof *d->edits * (size_t)d->edits_cap);
    if (!d->edits) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  d->edits[d->edits_n].at = at;
  d->edits[d->edits_n].text = strdup(text);
  d->edits_n++;
}

/* Does a statement left in place only define something (a method, an alias,
   a class macro), so that running it before a wrapped one changes nothing? */
static int sp_defer_defines(const pm_parser_t *parser, const pm_node_t *st) {
  switch (PM_NODE_TYPE(st)) {
    case PM_DEF_NODE: case PM_ALIAS_METHOD_NODE: case PM_ALIAS_GLOBAL_VARIABLE_NODE:
    case PM_UNDEF_NODE: case PM_SINGLETON_CLASS_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE:
      return 1;
    case PM_CALL_NODE: {
      const pm_call_node_t *call = (const pm_call_node_t *)st;
      if (!call->receiver || PM_NODE_TYPE(call->receiver) == PM_SELF_NODE) return 1;
      int rt = PM_NODE_TYPE(call->receiver);
      return (rt == PM_CONSTANT_READ_NODE || rt == PM_CONSTANT_PATH_NODE) &&
             sp_defer_shape_call(parser, call);
    }
    default: return 0;
  }
}

/* `scope` is the class path the body's methods are called through ("" at the
   top level), or NULL where it is not known (no statement is wrapped). Every
   statement left in place must keep its order with the wrapped ones: the
   file is not deferred when a run-time one (`ITEMS = Registry.all`) follows a
   wrapped one (`Registry.register :a`). */
static void sp_defer_body(SpDefer *d, const pm_node_t *body, const pm_constant_id_list_t *locals,
                          const char *scope) {
  if (!body || PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) return;
  const pm_node_list_t *list = &((const pm_statements_node_t *)body)->body;
  for (size_t i = 0; i < list->size; i++) {
    const pm_node_t *st = list->nodes[i];
    int t = PM_NODE_TYPE(st);
    if (t == PM_CLASS_NODE || t == PM_MODULE_NODE) {
      const pm_node_t *cp = t == PM_CLASS_NODE ? ((const pm_class_node_t *)st)->constant_path
                                               : ((const pm_module_node_t *)st)->constant_path;
      const pm_node_t *inner = t == PM_CLASS_NODE ? ((const pm_class_node_t *)st)->body
                                                  : ((const pm_module_node_t *)st)->body;
      const pm_constant_id_list_t *il = t == PM_CLASS_NODE ? &((const pm_class_node_t *)st)->locals
                                                           : &((const pm_module_node_t *)st)->locals;
      /* `module A` nests under the enclosing path; `module A::B` is only
         certain at the top level, where it is `::A::B` */
      char path[512];
      const char *sub = NULL;
      size_t cl = (size_t)(cp->location.end - cp->location.start);
      const char *ct = (const char *)cp->location.start;
      if (scope && (PM_NODE_TYPE(cp) == PM_CONSTANT_READ_NODE || !*scope) && cl < 200 &&
          strlen(scope) + cl + 4 < sizeof path) {
        snprintf(path, sizeof path, "%s%s%.*s", scope, (cl >= 2 && ct[0] == ':') ? "" : "::", (int)cl, ct);
        sub = path;
      }
      sp_defer_body(d, inner, il, sub);
      continue;
    }
    if (!scope || !sp_defer_stmt(d->parser, st, locals, !*scope)) {
      if (d->n && !sp_defer_defines(d->parser, st)) d->out_of_order = 1;
      continue;
    }
    char head[600], call[600];
    int k = d->n++;
    if (*scope) {
      snprintf(head, sizeof head, "def self.__spinel_load_%d_%d; ", d->file_no, k);
      snprintf(call, sizeof call, "%s.__spinel_load_%d_%d; ", scope, d->file_no, k);
    }
    else {
      snprintf(head, sizeof head, "def __spinel_load_%d_%d; ", d->file_no, k);
      snprintf(call, sizeof call, "__spinel_load_%d_%d; ", d->file_no, k);
    }
    sp_defer_edit(d, (size_t)(st->location.start - (const uint8_t *)d->source), head);
    sp_defer_edit(d, (size_t)(st->location.end - (const uint8_t *)d->source), "; end");
    size_t cl = strlen(call);
    if (d->calls_len + cl + 1 > d->calls_cap) {
      d->calls_cap = (d->calls_len + cl + 1) * 2;
      d->calls = realloc(d->calls, d->calls_cap);
      if (!d->calls) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    memcpy(d->calls + d->calls_len, call, cl + 1);
    d->calls_len += cl;
  }
}

/* The files a hoisted require deferred, by canonical path, with the
   expression a require of each answers: a later require of the same file,
   wherever it stands, runs what the first one held back. */
static char **sp_defer_paths, **sp_defer_vals;
static int sp_defer_count, sp_defer_cap;

static const char *sp_defer_lookup(const char *canonical) {
  for (int i = 0; canonical && i < sp_defer_count; i++)
    if (strcmp(sp_defer_paths[i], canonical) == 0) return sp_defer_vals[i];
  return NULL;
}

static void sp_defer_record(const char *canonical, const char *val) {
  sp_defer_paths = sp_grow_strs(sp_defer_paths, sp_defer_count, &sp_defer_cap, 8);
  sp_defer_vals = realloc(sp_defer_vals, sizeof(char *) * (size_t)sp_defer_cap);
  if (!sp_defer_vals) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_defer_paths[sp_defer_count] = strdup(canonical);
  sp_defer_vals[sp_defer_count++] = strdup(val);
}

/* Wrap `*content`'s run-time statements (above) and answer the expression
   the require's call becomes, recorded under the file's names `canonical`
   and `alt` (either may be NULL); NULL when the file has none and the
   require's value stays as it was. The files this one inlined were marked
   from `incl_from` on: one whose own statements were not deferred apart
   (it stood at this file's margin) would be wrapped with this file and
   answer nothing to a later require of its own, so such a file is left as
   it was. The flag is cleared when a wrapped statement raises, so a later
   require runs them again, as CRuby loads a file that failed again. */
static const char *sp_defer_runtime(char **content, const char *canonical, const char *alt,
                                    int incl_from) {
  static int file_no = 0;
  for (int i = incl_from; i < sp_included_count; i++) {
    const char *ip = sp_included_paths[i];
    if (!ip || (canonical && strcmp(ip, canonical) == 0) || (alt && strcmp(ip, alt) == 0)) continue;
    if (!sp_defer_lookup(ip)) return NULL;
  }
  const char *src = *content;
  pm_parser_t parser;
  sp_pm_parser_init(&parser, src, strlen(src), canonical);
  pm_node_t *root = pm_parse(&parser);
  SpDefer d;
  memset(&d, 0, sizeof d);
  d.parser = &parser; d.source = src; d.file_no = file_no;
  if (parser.error_list.size == 0 && PM_NODE_TYPE(root) == PM_PROGRAM_NODE) {
    const pm_program_node_t *prog = (const pm_program_node_t *)root;
    int top_return = 0;
    pm_visit_node(root, sp_defer_top_return, &top_return);
    if (!top_return) sp_defer_body(&d, (const pm_node_t *)prog->statements, &prog->locals, "");
  }
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  if (!d.n || d.out_of_order) {
    for (int i = 0; i < d.edits_n; i++) free(d.edits[i].text);
    free(d.edits); free(d.calls);
    return NULL;
  }
  file_no++;
  /* apply the edits, which are in source order */
  size_t slen = strlen(src), add = 0;
  for (int i = 0; i < d.edits_n; i++) add += strlen(d.edits[i].text);
  char *out = malloc(slen + add + 1);
  if (!out) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t o = 0, from = 0;
  for (int i = 0; i < d.edits_n; i++) {
    size_t at = d.edits[i].at, tl = strlen(d.edits[i].text);
    memcpy(out + o, src + from, at - from); o += at - from; from = at;
    memcpy(out + o, d.edits[i].text, tl); o += tl;
    free(d.edits[i].text);
  }
  memcpy(out + o, src + from, slen - from + 1);
  free(d.edits);
  free(*content);
  *content = out;
  size_t vcap = d.calls_len + 256;
  char *val = malloc(vcap);
  if (!val) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  snprintf(val, vcap, "($__spinel_loaded_%d ? false : ($__spinel_loaded_%d = true; "
           "begin; %srescue Exception; $__spinel_loaded_%d = nil; raise; end; true))",
           d.file_no, d.file_no, d.calls, d.file_no);
  free(d.calls);
  const char *ret = NULL;
  if (canonical) { sp_defer_record(canonical, val); ret = sp_defer_vals[sp_defer_count - 1]; }
  if (alt && (!canonical || strcmp(alt, canonical) != 0)) {
    sp_defer_record(alt, val);
    if (!ret) ret = sp_defer_vals[sp_defer_count - 1];
  }
  free(val);
  return ret;
}

static char *resolve_requires(const char *source, const char *source_path,
                              unsigned char **fsl_out, size_t *fsl_n_out) {
  /* Get base directory */
  char *path_copy = strdup(source_path);
  char *dir = strdup(path_copy);
  /* Find last / */
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  else { free(dir); dir = strdup("."); }
  free(path_copy);

  /* The file parsed by itself, each time it is read: its pragma, its
     encoding and its dead requires are read from this parse. */
  SpSrcFile file;
  sp_src_file_parse(&file, source, source_path);
  int file_fsl = sp_src_file_fsl(&file);
  sp_src_file_warn_encoding(&file);
  size_t code_len = sp_src_file_code_len(&file);
  sp_require_note_text(source, code_len);
  /* computed requires spelling a literal path (#5700), then autoloads with
     a literal path (#5696): each answers a fresh copy of what it reads */
  char *reachable = sp_rewrite_dead_requires(&file, code_len);
  sp_src_file_free(&file);
  char *pre_auto = sp_rewrite_computed_requires(reachable, dir);
  char *result = sp_rewrite_autoloads(pre_auto, dir);
  if (strcmp(reachable, result) != 0) g_require_unread = 1;   /* a require the two made */
  free(reachable);
  free(pre_auto);
  sp_autoload_is_main = 0;
  /* One pragma flag per line of `result`, kept in lockstep with every text
     splice below so each literal keeps its own file's flag. */
  size_t fsl_n;
  unsigned char *fsl = sp_fsl_make(result, file_fsl, &fsl_n);
  /* Same scan the plain requires use: wherever the call stands, and never the
     word as it appears inside a string, comment or heredoc body. */
  for (;;) {
    SpReqHit hit;
    if (!sp_require_find(result, result, "require_relative", &hit)) break;
    /* a file inlined under a condition is loaded only when that branch runs:
       it (and what it requires) stays loadable elsewhere */
    int incl_mark = sp_included_count, cond_inline = 0, ml = 0;
    char *pos = hit.kw, *expr_end = hit.expr_end;
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);
    /* Issue #765: bail rather than silently truncating overlong paths
       into the fixed-size buffers. */
    if (hit.arg_len >= 512) break;
    char rel_path[512];
    snprintf(rel_path, sizeof(rel_path), "%.*s", (int)hit.arg_len, hit.arg);
    /* What the call answers: true when this is the require that read the
       file, false when something already had (#3453). */
    const char *req_val = "true";
    /* a name computed at run time: CRuby's LoadError (as for require) */
    if (hit.arg[-1] == '"' && strstr(rel_path, "#{")) {
      char val[700];
      snprintf(val, sizeof val, "raise(LoadError, \"cannot load such file -- %s\")", rel_path);
      size_t cn = 0; unsigned char *cf = sp_fsl_make("", 0, &cn);
      sp_req_hoist_splice(&result, &fsl, &fsl_n, &hit, "", cf, cn, val);
      free(cf);
      continue;
    }
    /* (a file it names that is not there stops the compile below) */
    {
      size_t rl = strlen(rel_path);
      char rn[512];
      snprintf(rn, sizeof rn, "%s", rel_path);
      if (rl > 3 && strcmp(rn + rl - 3, ".rb") == 0) rn[rl - 3] = '\0';
      sp_note_required_feature(rn[0] == '.' && rn[1] == '/' ? rn + 2 : rn);
    }

    /* Build full path */
    char full_path[1024];
    /* an absolute path is taken as it is, as CRuby does */
    int fp_n = rel_path[0] == '/' ? snprintf(full_path, sizeof(full_path), "%s", rel_path)
                                  : snprintf(full_path, sizeof(full_path), "%s/%s", dir, rel_path);
    if (fp_n < 0 || (size_t)fp_n >= sizeof(full_path)) break;
    /* Collapse "." / ".." before appending ".rb", so a target ending in
       ".." resolves to the parent dir's `<dir>.rb` rather than "...rb". */
    sp_normalize_dots(full_path);
    {
      size_t fl = strlen(full_path);
      if (fl < sizeof(full_path) - 4 && (fl < 3 || strcmp(full_path + fl - 3, ".rb") != 0))
        strcat(full_path, ".rb");
    }

    char *canonical = sp_canonical_path(full_path);
    char *canon_keep = strdup(canonical);
    char *content;
    unsigned char *cfsl = NULL; size_t cfsl_n = 0;   /* spliced content's flags */
    /* the run-time part a hoisted require held back (sp_defer_runtime) */
    const char *dval = NULL;
    if (sp_path_already_included(canonical)) {
      /* Already inlined once -- replace require with empty content */
      content = strdup("# require_relative skipped (already included)");
      cfsl = sp_fsl_make(content, 0, &cfsl_n);
      dval = sp_defer_lookup(canonical);
      free(canonical);
      req_val = dval ? dval : "false";
      sp_req_resolved++;
    }
else {
      sp_mark_path_included(canonical);
      content = read_file(full_path);
      if (!content) {
        /* CRuby raises LoadError for a missing require_relative target. Spinel
           inlines at parse time, so a missing file means the classes/methods it
           was meant to define are simply absent -- and because dispatch then
           silently mis-resolves a call to a same-named method on another class
           (no missing-file error at the call site), the failure surfaces far
           from its cause. Halt the compile instead, matching CRuby. */
        fprintf(stderr,
                "spinel: %s: cannot load such file -- %s (require_relative \"%s\")\n",
                source_path, full_path, rel_path);
        exit(1);
      }
else {
        /* Recursively resolve */
        char *resolved = resolve_requires(content, full_path, &cfsl, &cfsl_n);
        free(content);
        content = resolved;
        sp_req_resolved++;
      }
      free(canonical);
    }

    /* Debug: wrap with PUSH/POP markers so the line map can attribute this
       file's nodes to the right source. No-op outside --debug. */
    content = sp_wrap_included(content, full_path);
    sp_fsl_pad_wrap(&cfsl, &cfsl_n);

    /* The call's value is live (a condition, an assignment, a receiver) or a
       trailing modifier would be stranded: inline the file ahead of the line
       and leave the value in the call's place (#3454). */
    {
      char *trail = expr_end;
      while (*trail == ' ' || *trail == '\t') trail++;
      int has_modifier = (*trail != '\n' && *trail != '\r' && *trail != ';' &&
                          *trail != '\0' && *trail != '#');
      ml = sp_req_if_modifier(expr_end);
      cond_inline = !hit.margin || ml || sp_in_cond_region(result, hit.kw);
      /* a later require of a file a hoisted one deferred stands where it
         was, and runs what that one held back */
      if (dval) {
        free(content); content = strdup("");
        free(cfsl); cfsl = sp_fsl_make(content, 0, &cfsl_n);
      }
      if (dval || (!hit.margin && !sp_req_in_control_only(result, hit.kw)) || (has_modifier && !ml)) {
        if (!dval && strcmp(req_val, "true") == 0) {
          const char *v = sp_defer_runtime(&content, canon_keep, NULL, incl_mark);
          if (v) req_val = v;
        }
        sp_req_hoist_splice(&result, &fsl, &fsl_n, &hit, content, cfsl, cfsl_n, req_val);
        free(cfsl);
        free(content);
        free(canon_keep);
        continue;
      }
    }
    free(canon_keep);

    /* a conditional inclusion is marked, so the requires inside it count as
       conditional too (the markers are comments: two lines, no flags) */
    if (cond_inline && (!hit.margin || ml)) content = sp_req_cond_wrap(content, &cfsl, &cfsl_n, expr_end, ml);

    /* Replace the statement, consuming its trailing whitespace and single
       newline; anything else on the line (`require_relative 'x'; code`) stays,
       pushed onto its own line by the inserted content's trailing newline. */
    char *stmt_end = expr_end + ml;
    while (*stmt_end == ' ' || *stmt_end == '\t') stmt_end++;
    if (*stmt_end == '\n') stmt_end++;
    size_t line_len = (size_t)(stmt_end - pos);
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;

    /* Mirror the splice on the flag buffer: the require line's one entry is
       replaced by the included file's entries. */
    sp_fsl_splice(&fsl, &fsl_n, sp_fsl_line_at(result, before_len), 1, cfsl, cfsl_n);
    free(cfsl);

    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);

    free(result);
    result = new_result;
    if (cond_inline)
      while (sp_included_count > incl_mark) free(sp_included_paths[--sp_included_count]);
    free(content);
  }
  free(dir);
  *fsl_out = fsl;
  *fsl_n_out = fsl_n;
  return result;
}

/* A `require "X"` whose X has no bundled lib/X.rb may still be provided
   natively by the Spinel runtime/codegen (e.g. the JSON module). Such a
   require is a harmless no-op and must not warn. Bundled .rb libs (set,
   stringio, strscan, erb, ...) are resolved by file existence and never
   reach the not-found branch, so they don't belong here -- only the
   C-native modules that map to a stdlib require name do. */
static int sp_lib_is_native(const char *name) {
  /* "json" moved to packages/json (a native binding, resolved by file) */
  static const char *const natives[] = { "io/console", "monitor", "time", "socket", "ostruct", NULL };
  for (int i = 0; natives[i]; i++) {
    if (strcmp(name, natives[i]) == 0) return 1;
  }
  return 0;
}

/* A `require "X"` for a capability Spinel provides as core, without a bundled
   file -- Thread/Mutex/Queue, Enumerator, Fiber. Modern CRuby treats these as
   no-ops (the feature is already loaded), so the require-gate tolerates them
   rather than failing with "cannot load such file". */
static int sp_require_tolerated(const char *name) {
  /* "ffi", where the bundled package is not built (no libffi): the builtin
     FFI DSL accepts the ffi gem's spellings (attach_function/callback,
     :string/:pointer/... types, extend FFI::Library as a no-op).
     "rational" / "complex": both classes are built in, so the require has
     nothing to load and nothing to warn about -- CRuby satisfies them
     silently for the same reason (#3455).
     "rbconfig" / "rubygems": the parser splices builtins/rbconfig.rb and
     builtins/gem.rb into a program that names RbConfig or Gem, and CRuby has
     both loaded before the program starts, so the require answers false.
     Refused under --require-gate, they broke every spin build using the
     bundled optparse, which requires rbconfig (#9441). ("gem" is no library
     in CRuby either: its require stays a LoadError there.) */
  static const char *const tolerated[] = { "thread", "enumerator", "fiber", "ffi",
                                           "rational", "complex", "io/wait",
                                           "rbconfig", "rubygems", NULL };
  for (int i = 0; tolerated[i]; i++) {
    if (strcmp(name, tolerated[i]) == 0) return 1;
  }
  return 0;
}

/* A library CRuby already has loaded when the program starts, so its require
   answers false rather than true even the first time it is written. Spinel is
   in the same position for these: Set is spliced into every program that uses
   it, so "already available" is the accurate answer here too (#3453). */
static int sp_require_preloaded(const char *name) {
  static const char *const preloaded[] = { "set", NULL };
  for (int i = 0; preloaded[i]; i++) {
    if (strcmp(name, preloaded[i]) == 0) return 1;
  }
  return 0;
}

/* The name a plain require looks up under packages/, or NULL to skip the
   lookup. packages/time/time.rb is CRuby's time.rb, for Time.parse and
   Time.strptime: a plain `require "time"` stays the runtime's, whose
   formatters and strict parsers are C, so a program that does not parse
   keeps the C it had (#9946). The file is reached only through
   SP_TIME_PARSERS_FEATURE, the name the time.rb splice requires it by:
   `require "time"` itself is already resolved by then, and a second one
   would be skipped as a repeat. */
#define SP_TIME_PARSERS_FEATURE "spinel/time_parsers"
static const char *sp_package_feature(const char *lib_name) {
  if (strcmp(lib_name, "time") == 0) return NULL;
  if (strcmp(lib_name, SP_TIME_PARSERS_FEATURE) == 0) return "time";
  return lib_name;
}

/* ---- Plain require resolution ---- */
static char *resolve_plain_requires(char *source, const char *exe_path,
                                    unsigned char **fsl, size_t *fsl_n);

static void sp_lib_dir(const char *exe_path, char *lib_dir, size_t lib_dir_n);
/* ---- builtins/: the core methods written in Ruby ----
   The names builtins/enumerable.rb defines, read once: the analyzer rewrites
   a call to one of them on an Enumerable receiver into a call of the
   top-level function the definition becomes (desugar_builtins). */
char **sp_builtin_enum_names = NULL;
int sp_builtin_enum_names_n = 0;

/* Read the `def name` (2-space indent, one container body deep) spellings
   out of a builtins/*.rb file into *names_out/*names_n_out. Shared by
   enumerable.rb's own global table and the other containers' per-file ones
   below (sp_builtin_extras). */
static void sp_builtin_names_from_into(const char *content, char ***names_out, int *names_n_out) {
  const char *p = content;
  while ((p = strstr(p, "\n  def ")) != NULL) {
    p += 7;
    const char *q = p;
    while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_') q++;
    if (*q == '?' || *q == '!') q++;
    if (q == p) continue;
    char *nm = (char *)malloc((size_t)(q - p) + 1);
    if (!nm) return;
    memcpy(nm, p, (size_t)(q - p)); nm[q - p] = 0;
    *names_out = (char **)realloc(*names_out, sizeof(char *) * (size_t)(*names_n_out + 1));
    (*names_out)[(*names_n_out)++] = nm;
    p = q;
  }
}

static void sp_builtin_names_from(const char *content) {
  sp_builtin_names_from_into(content, &sp_builtin_enum_names, &sp_builtin_enum_names_n);
}

/* ---- what prism's lexer says of the text the builtin splices ask about ----
   The splices below are decided before the parse, from the resolved text,
   and the text alone cannot say where code stops: a `#` starts a comment
   only where the lexer is reading code. In a string literal, a heredoc
   body, a regexp or a %-literal it is a character, and as `#{` it opens
   code. Nor can it say whether a word is a token or part of a string. So
   those two questions go to prism, once per text and only when a splice has
   to ask: the comments it found, and whether `break` and `define_finalizer`
   were lexed as the keyword and an identifier (inside an interpolation too).
   A program that evaluates a string has code prism lexed as text (a
   class_eval template), so there a word counts wherever it is spelled, as
   it did before; so does one prism could not parse. */
typedef struct {
  const char *src;            /* the text this was read from, NULL for none */
  size_t *com; int com_n;     /* each comment's start and end offset, in order */
  size_t *mods; int mods_n, mods_cap; /* each `module` keyword's offset, in order */
  int brk, fin, evals, popen, popen_str;
} SpSrcLex;
static SpSrcLex sp_src_lex;

static int sp_src_tok_is(const pm_token_t *tok, const char *word) {
  size_t wl = strlen(word);
  return (size_t)(tok->end - tok->start) == wl && memcmp(tok->start, word, wl) == 0;
}

static int sp_src_top_io(const uint8_t *src, const uint8_t *p) {
  size_t off = (size_t)(p - src);
  if (off >= 2 && p[-1] == ':' && p[-2] == ':') return off == 2 || !sp_req_ident_char((char)p[-3]);
  return off == 0 || (p[-1] != ':' && !sp_req_ident_char((char)p[-1]));
}

static void sp_src_lex_token(void *data, pm_parser_t *parser, pm_token_t *tok) {
  SpSrcLex *lx = (SpSrcLex *)data;
  (void)parser;
  if (tok->type == PM_TOKEN_KEYWORD_BREAK) lx->brk = 1;
  if (tok->type == PM_TOKEN_KEYWORD_MODULE) {
    if (lx->mods_n == lx->mods_cap) {
      lx->mods_cap = lx->mods_cap ? lx->mods_cap * 2 : 64;
      lx->mods = (size_t *)realloc(lx->mods, sizeof(size_t) * (size_t)lx->mods_cap);
      if (!lx->mods) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    lx->mods[lx->mods_n++] = (size_t)(tok->start - (const uint8_t *)lx->src);
  }
  for (const uint8_t *q = tok->start; tok->type == PM_TOKEN_STRING_CONTENT && q + 8 <= tok->end; q++)
    if (!memcmp(q, "IO.popen", 8)) lx->popen_str = 1;
  if (tok->type != PM_TOKEN_IDENTIFIER) return;
  if (sp_src_tok_is(tok, "define_finalizer")) lx->fin = 1;
  if (sp_src_tok_is(tok, "popen") && tok->start - (const uint8_t *)lx->src >= 3 && !memcmp(tok->start - 3, "IO.", 3) &&
      sp_src_top_io((const uint8_t *)lx->src, tok->start - 3)) lx->popen = 1;
  if (sp_src_tok_is(tok, "eval") || sp_src_tok_is(tok, "class_eval") ||
      sp_src_tok_is(tok, "module_eval") || sp_src_tok_is(tok, "instance_eval")) lx->evals = 1;
}

/* The text is about to be freed or replaced: what was read from it goes. */
static void sp_src_lex_drop(void) {
  free(sp_src_lex.com);
  free(sp_src_lex.mods);
  memset(&sp_src_lex, 0, sizeof sp_src_lex);
}

static int sp_src_off_cmp(const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return (x > y) - (x < y);
}

static SpSrcLex *sp_src_lex_of(const char *source) {
  SpSrcLex *lx = &sp_src_lex;
  if (lx->src == source) return lx;
  sp_src_lex_drop();
  lx->src = source;
  pm_parser_t parser;
  pm_lex_callback_t cb = { lx, sp_src_lex_token };
  sp_pm_parser_init(&parser, source, strlen(source), NULL);
  parser.lex_callback = &cb;
  pm_node_t *root = pm_parse(&parser);
  int cap = 0;
  for (pm_comment_t *c = (pm_comment_t *)parser.comment_list.head; c; c = (pm_comment_t *)c->node.next) {
    if (lx->com_n + 2 > cap) {
      cap = cap ? cap * 2 : 64;
      lx->com = (size_t *)realloc(lx->com, sizeof(size_t) * (size_t)cap);
      if (!lx->com) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    lx->com[lx->com_n++] = (size_t)(c->location.start - (const uint8_t *)source);
    lx->com[lx->com_n++] = (size_t)(c->location.end - (const uint8_t *)source);
  }
  if (parser.error_list.size > 0) lx->evals = 1;
  /* the lexer reads a heredoc's body before the rest of its opening line,
     so a `module` in the body's code comes before one later on that line */
  if (lx->mods_n > 1) qsort(lx->mods, (size_t)lx->mods_n, sizeof(size_t), sp_src_off_cmp);
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  return lx;
}

/* Is the byte at `at` inside a comment of `source`? */
static int sp_src_in_comment(const char *source, const char *at) {
  SpSrcLex *lx = sp_src_lex_of(source);
  size_t off = (size_t)(at - source);
  int lo = 0, hi = lx->com_n / 2;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (off < lx->com[mid * 2]) hi = mid;
    else if (off >= lx->com[mid * 2 + 1]) lo = mid + 1;
    else return 1;
  }
  return 0;
}

/* Does the source mention `name` as a method: `.name` or a bare `name`
   followed by `(`, ` {`, ` do` or an argument, outside a comment? A textual
   test, as the Set splice's is: a false positive costs the parse of a small
   file, and the names never reach the generated C uncalled. */
static int sp_source_mentions_method(const char *src, const char *name) {
  size_t nl = strlen(name);
  const char *p = src;
  while ((p = strstr(p, name)) != NULL) {
    const char *after = p + nl;
    unsigned char ac = (unsigned char)*after;
    int word_end = !((ac >= 'a' && ac <= 'z') || (ac >= 'A' && ac <= 'Z') || (ac >= '0' && ac <= '9') || ac == '_' || ac == '?' || ac == '!' || ac == '=');
    unsigned char bc = p > src ? (unsigned char)p[-1] : ' ';
    /* a Symbol naming it (`send(:tally)`, `method(:tally)`, `&:tally`) calls
       it as surely as `.tally` does; `Foo::tally` is a constant path's */
    int sym = bc == ':' && p - src >= 2 && p[-2] != ':' && !(p - src >= 2 && sp_req_ident_char(p[-2]));
    int word_start = sym || !((bc >= 'a' && bc <= 'z') || (bc >= 'A' && bc <= 'Z') || (bc >= '0' && bc <= '9') || bc == '_' || bc == '@' || bc == '$' || bc == ':');
    p = after;
    if (!word_end || !word_start) continue;
    /* not in a comment: a `#` between the line start and the name is asked
       of the lexer, since one in a string literal or opening `#{` hides
       nothing (`puts "issue #12"; p a.partition { ... }`) */
    const char *ls = p - nl;
    while (ls > src && ls[-1] != '\n') ls--;
    int in_comment = 0;
    for (const char *k = ls; k < p - nl; k++) if (*k == '#') { in_comment = sp_src_in_comment(src, p - nl); break; }
    if (in_comment) continue;
    if (bc == '.' || sym) return 1;
    if (ac == '(' || ac == ' ' || ac == '\n') return 1;
  }
  return 0;
}

/* A builtin library CRuby has loaded before the program runs -- RubyGems'
   `Gem`, `RbConfig` -- that library code consults without a require. When the
   whole program (libraries included) names the constant (`Gem.x`, `Gem::X`)
   outside a comment and defines none of its own, builtins/<file> is spliced
   at the top. */
static int sp_src_names_const(const char *src, const char *w) {
  size_t wl = strlen(w);
  for (const char *p = strstr(src, w); p; p = strstr(p + 1, w)) {
    char prev = p == src ? 0 : p[-1];
    if (sp_req_ident_char(prev) || prev == '@' || prev == '$') continue;
    /* `Foo::Gem` is Foo's; a leading `::Gem` is the top-level one */
    if (prev == ':' && (p - src < 2 || p[-2] != ':' ||
                        (p - src >= 3 && (sp_req_ident_char(p[-3]) || p[-3] == ')'))))
      continue;
    if (p[wl] != '.' && !(p[wl] == ':' && p[wl + 1] == ':')) continue;
    const char *bol = p;
    while (bol > src && bol[-1] != '\n') bol--;
    while (*bol == ' ' || *bol == '\t') bol++;
    if (*bol == '#') continue;
    return 1;
  }
  return 0;
}
/* Does the source open `kw` ("module Enumerable", "class Set") as written,
   the name ending there? A bare strstr also took ruby/spec's
   `module EnumerableSpecs` or a `class Settings` for it. */
static int sp_src_opens(const char *source, const char *kw) {
  size_t kl = strlen(kw);
  for (const char *p = strstr(source, kw); p; p = strstr(p + 1, kw))
    if (!sp_req_ident_char(p[kl])) return 1;
  return 0;
}

/* Was the `module` at `at` lexed as the keyword? */
static int sp_src_module_keyword_at(const char *source, const char *at) {
  SpSrcLex *lx = sp_src_lex_of(source);
  size_t off = (size_t)(at - source);
  int lo = 0, hi = lx->mods_n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (lx->mods[mid] < off) lo = mid + 1;
    else hi = mid;
  }
  return lo < lx->mods_n && lx->mods[lo] == off;
}

/* Does the program open module `cname` itself (`module RbConfig`, the
   name ending there)? A `module RbConfig` in a comment opens nothing: a
   library's documentation comment kept builtins/rbconfig.rb out, and
   RbConfig::CONFIG was an uninitialized constant. Nor does one in a string
   literal or a heredoc, where the lexer read no keyword. A program that
   evaluates a string, or that prism could not parse, counts the text
   outside comments, as sp_src_lex_of says. */
static int sp_src_defines_module(const char *source, const char *cname) {
  char def[80];
  int dl = snprintf(def, sizeof def, "module %s", cname);
  if (dl <= 0 || (size_t)dl >= sizeof def) return 0;
  for (const char *p = strstr(source, def); p; p = strstr(p + 1, def)) {
    char after = p[dl];
    if (after != '\n' && after != ' ' && after != '\t' && after != '\r' && after != ';') continue;
    if (p > source && sp_req_ident_char(p[-1])) continue;
    if (sp_src_in_comment(source, p)) continue;
    if (!sp_src_lex_of(source)->evals && !sp_src_module_keyword_at(source, p)) continue;
    return 1;
  }
  return 0;
}

static char *sp_prepend_require(char *source, const char *exe_path, const char *head,
                                unsigned char **fsl, size_t *fsl_n) {
  size_t sl = strlen(source), hl = strlen(head);
  char *ns = (char *)malloc(sl + hl + 1);
  if (!ns) return source;
  memcpy(ns, head, hl); memcpy(ns + hl, source, sl + 1);
  sp_require_note_text(head, strlen(head));
  sp_src_lex_drop();
  free(source);
  ns = resolve_plain_requires(ns, exe_path, fsl, fsl_n);
  size_t pl = strlen(SP_PUSH_PREFIX), il = strlen(SP_INSERT_PREFIX), nl = strlen(ns);
  char *w = strncmp(ns, SP_PUSH_PREFIX, pl) == 0 ? (char *)malloc(nl - pl + il + 1) : NULL;
  if (!w) return ns;
  memcpy(w, SP_INSERT_PREFIX, il); memcpy(w + il, ns + pl, nl - pl + 1);
  free(ns);
  return w;
}

static char *sp_splice_named_builtin(char *source, const char *exe_path, const char *cname,
                                     const char *file, unsigned char **fsl, size_t *fsl_n) {
  if (!sp_src_names_const(source, cname) || sp_src_defines_module(source, cname)) return source;
  char head[96]; snprintf(head, sizeof head, "require \"%s\"\n", file);
  return sp_prepend_require(source, exe_path, head, fsl, fsl_n);
}

/* builtins/monitor_mixin.rb: MonitorMixin, for a program that names it in
   code (`include MonitorMixin`, `MonitorMixin::ConditionVariable`) and does
   not define it -- named, not called through, so the Gem-style test of a
   `.`/`::` after the name does not ask the right question */
static char *sp_splice_monitor_mixin(char *source, const char *exe_path,
                                     unsigned char **fsl, size_t *fsl_n) {
  static const char w[] = "MonitorMixin";
  size_t wl = sizeof w - 1;
  int named = 0;
  for (const char *p = strstr(source, w); p && !named; p = strstr(p + 1, w)) {
    char prev = p == source ? 0 : p[-1];
    if (sp_req_ident_char(prev) || prev == '@' || prev == '$' || sp_req_ident_char(p[wl])) continue;
    const char *bol = p;
    while (bol > source && bol[-1] != '\n') bol--;
    while (*bol == ' ' || *bol == '\t') bol++;
    if (*bol != '#') named = 1;
  }
  if (!named || sp_src_defines_module(source, w)) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/monitor_mixin\"\n", fsl, fsl_n);
}

/* builtins/object_space.rb: the finalizer API. Only for a program that names
   it -- the rest of ObjectSpace stays the refusal it is
   (docs/limitations.md). Names it in code, that is: the file holds procs in
   a table and calls them, which the whole-program analysis answers for, so a
   program that only prints the word was refused for what it never did. */
static char *sp_splice_object_space(char *source, const char *exe_path,
                                    unsigned char **fsl, size_t *fsl_n) {
  if (!strstr(source, "define_finalizer")) return source;
  { SpSrcLex *lx = sp_src_lex_of(source); if (!lx->fin && !lx->evals) return source; }
  if (sp_src_defines_module(source, "ObjectSpace")) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/object_space\"\n", fsl, fsl_n);
}

/* builtins/process_detach.rb: Process.detach, for a program that calls it
   and does not open Process itself (#7203) */
static char *sp_splice_process_detach(char *source, const char *exe_path,
                                      unsigned char **fsl, size_t *fsl_n) {
  if (!strstr(source, "Process.detach") || sp_src_defines_module(source, "Process")) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/process_detach\"\n", fsl, fsl_n);
}

/* builtins/system_spawn.rb: Kernel#system with an options or environment
   Hash, for a program that calls system with a keyword argument (the
   analysis renames those calls to it, desugar_system_options) */
static int sp_src_system_call_has_keyword(const char *source) {
  for (const char *p = strstr(source, "system"); p; p = strstr(p + 6, "system")) {
    if (p > source && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.' ||
                       p[-1] == ':' || p[-1] == '@' || p[-1] == '$')) continue;
    const char *q = p + 6;
    if (*q != '(' && *q != ' ') continue;
    /* the call's text: to the end of its line, and on through a line that
       ends inside the argument list (a trailing comma or open parenthesis) */
    for (; *q; q++) {
      if (*q == '\n') {
        const char *e = q;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        if (e > p && (e[-1] == ',' || e[-1] == '(')) continue;
        break;
      }
      /* `name:` and not `Name::` -- a keyword argument */
      if (*q == ':' && q[1] != ':' && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) return 1;
    }
  }
  return 0;
}

static char *sp_splice_system_spawn(char *source, const char *exe_path,
                                    unsigned char **fsl, size_t *fsl_n) {
  if (!sp_src_system_call_has_keyword(source)) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/system_spawn\"\n", fsl, fsl_n);
}

/* builtins/file_foreach_first.rb: File.foreach(path).first(n), for a program
   that writes `foreach(` and `.first(` (desugar_dir_surface renames the call) */
static char *sp_splice_foreach_first(char *source, const char *exe_path,
                                     unsigned char **fsl, size_t *fsl_n) {
  if (!strstr(source, "foreach(") || !strstr(source, ".first(")) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/file_foreach_first\"\n", fsl, fsl_n);
}

/* builtins/io_popen.rb: IO.popen with a block, for a program that calls
   IO.popen (the analysis renames the block form to it, desugar_io_popen_block) */
static char *sp_splice_io_popen(char *source, const char *exe_path,
                                unsigned char **fsl, size_t *fsl_n) {
  if (!strstr(source, "IO.popen")) return source;
  { SpSrcLex *lx = sp_src_lex_of(source); if (!lx->popen && !(lx->evals && lx->popen_str)) return source; }
  return sp_prepend_require(source, exe_path, "require \"builtins/io_popen\"\n", fsl, fsl_n);
}

static char *sp_splice_builtins(char *source, const char *exe_path,
                                unsigned char **fsl, size_t *fsl_n) {
  if (getenv("SPINEL_NO_BUILTINS")) return source;   /* the A/B switch: the C emitters alone */
  if (sp_builtin_enum_names_n == 0) {
    char lib_dir[1024], gp[1200];
    sp_lib_dir(exe_path, lib_dir, sizeof lib_dir);
    int base_len = (int)strlen(lib_dir);
    if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
    snprintf(gp, sizeof gp, "%.*s/builtins/enumerable.rb", base_len, lib_dir);
    char *content = read_file(gp);
    if (!content) { snprintf(gp, sizeof gp, "%.*s/../builtins/enumerable.rb", base_len, lib_dir); content = read_file(gp); }
    if (!content) {
      /* The C emitters for these methods are gone: a compiler that cannot
         find the file has no Enumerable#partition at all, and a program
         calling one compiled to an unconditional NoMethodError whose
         carrier then failed the C build two files away from the cause. A
         toolchain staged by hand (binary + lib/ + packages/, the list that
         was complete before this directory existed) is the way to get
         here; say so, where the binary looked. */
      fprintf(stderr,
              "spinel: builtins/enumerable.rb not found beside the compiler (looked under %.*s and its parent);\n"
              "        the toolchain is incomplete -- it ships with `make install`. SPINEL_NO_BUILTINS=1 compiles without it.\n",
              base_len, lib_dir);
      exit(1);
    }
    sp_builtin_names_from(content); free(content);
    if (sp_builtin_enum_names_n == 0) return source;
  }
  /* A program that reopens Enumerable gets the builtins beside it, unless
     its reopening may define a builtin's name itself: its own then has to
     answer for an Array or a Hash too, which the builtin would answer
     instead. Only the reopening's own body counts -- another class with a
     method of that name (a `select` of its own) takes nothing from
     Enumerable, and reading the whole program left the builtins out of
     every program that had one (activesupport's) */
  if (sp_src_opens(source, "module Enumerable"))
    for (const char *mo = strstr(source, "module Enumerable"); mo; mo = strstr(mo + 1, "module Enumerable")) {
      if (sp_req_ident_char(mo[17])) continue;
      const char *ls = mo;
      while (ls > source && ls[-1] != '\n') ls--;
      int ind = (int)(mo - ls);
      const char *body_end = strchr(mo, '\n');
      /* a one-line `module Enumerable; ...; end` is its own body */
      const char *semi = strchr(mo, ';');
      if (body_end && !(semi && semi < body_end)) {
        /* the body runs to the `end` at the module's own indentation */
        for (const char *ln = body_end + 1; *ln; ) {
          const char *nx = strchr(ln, '\n');
          int li = 0; while (ln[li] == ' ' || ln[li] == '\t') li++;
          if (li == ind && strncmp(ln + li, "end", 3) == 0 && !sp_req_ident_char(ln[li + 3])) { body_end = ln + li; break; }
          if (!nx) { body_end = ln + strlen(ln); break; }
          ln = nx + 1;
        }
      }
      if (!body_end) body_end = mo + strlen(mo);
      for (int i = 0; i < sp_builtin_enum_names_n; i++) {
        const char *nm = sp_builtin_enum_names[i];
        size_t nl = strlen(nm);
        for (const char *p = strstr(mo, "def "); p && p < body_end; p = strstr(p + 4, "def ")) {
          const char *q = p + 4;
          while (*q == ' ') q++;
          if (strncmp(q, nm, nl) == 0 && !(isalnum((unsigned char)q[nl]) || q[nl] == '_' ||
                                           q[nl] == '?' || q[nl] == '!' || q[nl] == '='))
            return source;
        }
      }
    }
  int any = 0;
  for (int i = 0; i < sp_builtin_enum_names_n && !any; i++)
    if (sp_source_mentions_method(source, sp_builtin_enum_names[i])) any = 1;
  /* the spellings the analyzer rewrites onto a builtin's name */
  static const char *const aliases[][2] = { { "with_object", "each_with_object" }, { "collect_concat", "flat_map" }, { "detect", "find" },
    /* Array#index with a block and no argument (desugar_builtin_enum_calls) */
    { "index", "find_index" }, { NULL, NULL } };
  for (int k = 0; aliases[k][0] && !any; k++) {
    int known = 0;
    for (int i = 0; i < sp_builtin_enum_names_n; i++) if (strcmp(sp_builtin_enum_names[i], aliases[k][1]) == 0) known = 1;
    if (known && sp_source_mentions_method(source, aliases[k][0])) any = 1;
  }
  if (!any) return source;
  return sp_prepend_require(source, exe_path, "require \"builtins/enumerable\"\n", fsl, fsl_n);
}

/* builtins/enumerator.rb: the Enumerator walks desugar_enum_walk_calls
   (analyze_desugar.c) moves a breaking block's call onto. It can only be
   wanted by a program that breaks out of a block given to one of the
   names it covers; like the Integer/Float/Comparable files, a missing file
   just never splices, and the calls stay on their typed emitters. */
static char *sp_splice_builtin_enumerator(char *source, const char *exe_path,
                                          unsigned char **fsl, size_t *fsl_n) {
  if (getenv("SPINEL_NO_BUILTINS")) return source;
  /* a String's piece walk with an index (str.each_char.with_index { }),
     which builtins/enumerator.rb's __enumw_str_*_with_index serve when the
     String is no variable to read again after the walk */
  int str_walk = sp_source_mentions_method(source, "with_index") &&
                 (sp_source_mentions_method(source, "each_char") || sp_source_mentions_method(source, "each_line") ||
                  sp_source_mentions_method(source, "each_byte") || sp_source_mentions_method(source, "each_codepoint"));
  if (!str_walk && !strstr(source, "break")) return source;
  static const char *const names[] = {
    "map", "collect", "select", "filter", "reject", "filter_map", "each_with_object", "with_object",
    "inject", "reduce", "each_slice", "each_cons", "each_entry", "with_index", NULL
  };
  int any = 0;
  for (int i = 0; names[i] && !any; i++) if (sp_source_mentions_method(source, names[i])) any = 1;
  if (!any) return source;
  /* the keyword, not the word in a string or a comment */
  { SpSrcLex *lx = sp_src_lex_of(source); if (!str_walk && !lx->brk && !lx->evals) return source; }
  char lib_dir[1024], gp[1200];
  sp_lib_dir(exe_path, lib_dir, sizeof lib_dir);
  int base_len = (int)strlen(lib_dir);
  if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
  snprintf(gp, sizeof gp, "%.*s/builtins/enumerator.rb", base_len, lib_dir);
  FILE *fp = fopen(gp, "r");
  if (!fp) { snprintf(gp, sizeof gp, "%.*s/../builtins/enumerator.rb", base_len, lib_dir); fp = fopen(gp, "r"); }
  if (!fp) return source;
  fclose(fp);
  return sp_prepend_require(source, exe_path, "require \"builtins/enumerator\"\n", fsl, fsl_n);
}

/* ---- builtins/: the other containers (Integer, Float, Comparable) ----
   Same splice-if-mentioned idea as enumerable.rb above, generalized to a
   small table so a new container is one more row here plus its own file
   under builtins/ and its own desugar container/prefix in
   analyze_desugar.c (desugar_builtin_scalar_defs / _calls). Two
   differences from enumerable.rb, both because these are optional and
   younger: (1) a missing file is not an error -- a container with no
   builtins/<name>.rb yet (Float, Comparable, until their own methods
   land) simply never splices, where enumerable.rb's absence is a broken
   toolchain; (2) each file gets its own `require` line, spliced
   independently, so a program using only Integer names never parses
   float.rb or comparable.rb at all. SPINEL_NO_BUILTINS (checked at the
   top of sp_splice_builtins above) already gates the call site below, so
   both mechanisms share the one A/B switch. */
typedef struct {
  const char *file;   /* "builtins/integer.rb" */
  const char *req;    /* the require name: "builtins/integer" */
  char **names;
  int names_n;
  int scanned;         /* the file load was attempted (found or not) */
} SpBuiltinExtra;

static SpBuiltinExtra sp_builtin_extras[] = {
  { "builtins/integer.rb", "builtins/integer", NULL, 0, 0 },
  { "builtins/float.rb", "builtins/float", NULL, 0, 0 },
  { "builtins/comparable.rb", "builtins/comparable", NULL, 0, 0 },
};
#define SP_BUILTIN_EXTRA_N ((int)(sizeof(sp_builtin_extras) / sizeof(sp_builtin_extras[0])))

static char *sp_splice_builtin_extra(char *source, const char *exe_path, SpBuiltinExtra *bf,
                                      unsigned char **fsl, size_t *fsl_n) {
  if (!bf->scanned) {
    bf->scanned = 1;
    char lib_dir[1024], gp[1200];
    sp_lib_dir(exe_path, lib_dir, sizeof lib_dir);
    int base_len = (int)strlen(lib_dir);
    if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
    snprintf(gp, sizeof gp, "%.*s/%s", base_len, lib_dir, bf->file);
    char *content = read_file(gp);
    if (!content) { snprintf(gp, sizeof gp, "%.*s/../%s", base_len, lib_dir, bf->file); content = read_file(gp); }
    if (content) { sp_builtin_names_from_into(content, &bf->names, &bf->names_n); free(content); }
  }
  if (bf->names_n == 0) return source;
  int any = 0;
  for (int i = 0; i < bf->names_n && !any; i++)
    if (sp_source_mentions_method(source, bf->names[i])) any = 1;
  if (!any) return source;
  char head[160]; snprintf(head, sizeof head, "require \"%s\"\n", bf->req);
  return sp_prepend_require(source, exe_path, head, fsl, fsl_n);
}

static char *sp_splice_builtin_extras(char *source, const char *exe_path,
                                       unsigned char **fsl, size_t *fsl_n) {
  if (getenv("SPINEL_NO_BUILTINS")) return source;
  for (int i = 0; i < SP_BUILTIN_EXTRA_N; i++)
    source = sp_splice_builtin_extra(source, exe_path, &sp_builtin_extras[i], fsl, fsl_n);
  return source;
}

/* Accessors for analyze_desugar.c (desugar_builtin_scalar_defs/_calls):
   sp_builtin_extras is static to this file, and its `names` arrays are
   only populated lazily (the file is read on first use, from
   sp_splice_builtin_extra above), which by the time analysis runs has
   already happened for every extra a program's source could have
   mentioned. Container indices match SP_BUILTIN_EXTRA_N's table order:
   0 = Integer, 1 = Float, 2 = Comparable. */
int sp_builtin_extra_names_n(int idx) {
  return (idx >= 0 && idx < SP_BUILTIN_EXTRA_N) ? sp_builtin_extras[idx].names_n : 0;
}
int sp_builtin_extra_name_index(int idx, const char *name) {
  if (!name || idx < 0 || idx >= SP_BUILTIN_EXTRA_N) return -1;
  for (int i = 0; i < sp_builtin_extras[idx].names_n; i++)
    if (strcmp(sp_builtin_extras[idx].names[i], name) == 0) return i;
  return -1;
}

/* lib/ relative to this executable, resolving symlinks the same way the
   compiler locates its runtime (#1663): an installed tree runs through the
   /usr/local/bin/spinel symlink, and dirname(argv0) would point the
   bundled-gem lookup at bin/. */
static void sp_lib_dir(const char *exe_path, char *lib_dir, size_t lib_dir_n) {
  {
    char self[1024];
    self[0] = '\0';
#if defined(__APPLE__)
    uint32_t bsz = (uint32_t)sizeof self;
    if (_NSGetExecutablePath(self, &bsz) != 0) self[0] = '\0';
    if (self[0]) {
      char rp[1024];
      if (realpath(self, rp)) snprintf(self, sizeof self, "%s", rp);
    }
#else
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) self[n] = '\0'; else self[0] = '\0';
#endif
    if (!self[0]) {
      char rp[1024];
      if (exe_path && realpath(exe_path, rp)) snprintf(self, sizeof self, "%s", rp);
      else snprintf(self, sizeof self, "%s", exe_path ? exe_path : ".");
    }
    strncpy(lib_dir, self, lib_dir_n - 1);
    lib_dir[lib_dir_n - 1] = '\0';
  }
  char *slash = strrchr(lib_dir, '/');
  if (slash) *slash = '\0';
  else strcpy(lib_dir, ".");
  strcat(lib_dir, "/lib");
}

static char *resolve_plain_requires(char *source, const char *exe_path,
                                    unsigned char **fsl, size_t *fsl_n) {
  char lib_dir[1024];
  sp_lib_dir(exe_path, lib_dir, sizeof lib_dir);

  char *result = source;
  /* The scan finds a `require` wherever it stands -- at the margin, indented
     inside a body, or in the middle of an expression -- and skips the ones a
     string, comment or heredoc only appears to contain. A computed argument
     (`require File.expand_path('x', __dir__)`) matches nothing and is left
     alone: grabbing the first quote on the line would mistake an inner string
     literal for the lib name and strand the rest of the expression (#1383).
     Each splice rebuilds the buffer, so the scan restarts from the top; the
     stubs it leaves behind are comments, which the scan skips. */
  for (;;) {
    SpReqHit hit;
    if (!sp_require_find(result, result, "require", &hit)) break;
    /* a file inlined under a condition is loaded only when that branch runs:
       it (and what it requires) stays loadable elsewhere */
    int incl_mark = sp_included_count, cond_inline = 0, ml = 0;
    char *pos = hit.kw, *expr_end = hit.expr_end;
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    char lib_name[256];
    if (hit.arg_len >= sizeof lib_name) break;
    snprintf(lib_name, sizeof(lib_name), "%.*s", (int)hit.arg_len, hit.arg);
    /* `require "webrick/compat.rb"` names the same feature as
       `require "webrick/compat"`: each lookup below adds the extension */
    {
      size_t ln = strlen(lib_name);
      if (ln > 3 && strcmp(lib_name + ln - 3, ".rb") == 0) lib_name[ln - 3] = '\0';
    }
    /* What the call answers, in CRuby's terms: true when this require is what
       loads the library, false when it was already there (a repeat, or a
       capability Spinel provides without loading anything), and nil for a
       require Spinel could not satisfy at all -- where CRuby would have raised
       LoadError rather than answer (#3453). */
    const char *req_val = "nil";
    int root_dup = 0;
    /* A name computed at run time (`require "sqlite3/#{v}/native"`) names no
       file the program was compiled with: the call raises CRuby's LoadError,
       with the message it would carry, which the usual `rescue LoadError`
       fallback around it expects. */
    if (hit.arg[-1] == '"' && strstr(lib_name, "#{")) {
      char val[400];
      snprintf(val, sizeof val, "raise(LoadError, \"cannot load such file -- %s\")", lib_name);
      size_t cn = 0; unsigned char *cf = sp_fsl_make("", 0, &cn);
      sp_req_hoist_splice(&result, fsl, fsl_n, &hit, "", cf, cn, val);
      free(cf);
      continue;
    }
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, lib_name);
    {
      size_t fl = strlen(lib_path);
      if (fl < sizeof(lib_path) - 4 && (fl < 3 || strcmp(lib_path + fl - 3, ".rb") != 0))
        strcat(lib_path, ".rb");
    }

    /* Same dedup as resolve_requires: a file pulled in via plain `require`
       must not be re-inlined if a previous `require` or `require_relative`
       already pulled it. Otherwise mixing the two forms for the same lib
       still produces struct-redefinition errors. */
    char *canonical = sp_canonical_path(lib_path);
    char *canon_keep = strdup(canonical);
    char *content;
    unsigned char *cfsl = NULL; size_t cfsl_n = 0;   /* spliced content's flags */
    /* the run-time part a hoisted require held back (sp_defer_runtime) */
    const char *dval = NULL;
    if (sp_path_already_included(canonical)) {
      content = strdup("# require skipped (already included)");
      dval = sp_defer_lookup(canonical);
      free(canonical);
      req_val = dval ? dval : "false";
      sp_req_resolved++;
    }
else {
      sp_mark_path_included(canonical);
      free(canonical);
      content = read_file(lib_path);
      if (!content) {
        /* the compiler binary may live one level below the repo root (e.g.
           build/spinel with the stdlib at ../lib): retry one level up */
        char alt_path[1024];
        int exe_len = (int)strlen(lib_dir) - 4;   /* strip the trailing "/lib" */
        if (exe_len < 0) exe_len = 0;
        snprintf(alt_path, sizeof(alt_path), "%.*s/../lib/%s", exe_len, lib_dir, lib_name);
        size_t al = strlen(alt_path);
        if (al < sizeof(alt_path) - 4 && (al < 3 || strcmp(alt_path + al - 3, ".rb") != 0))
          strcat(alt_path, ".rb");
        content = read_file(alt_path);
        if (content) snprintf(lib_path, sizeof(lib_path), "%s", alt_path);
      }
      if (!content) {
        /* `-I <dir>` feature roots: <root>/X.rb, else <root>/X/<last>.rb. They
           come before the pre-installed packages, so a project's package of the
           same name as a bundled one is the one a require reaches (#7207); lib/
           stays first. */
        char rp[1024];
        const char *last = strrchr(lib_name, '/');
        last = last ? last + 1 : lib_name;
        for (int ri = 0; ri < sp_feature_roots_n && !content; ri++) {
          snprintf(rp, sizeof(rp), "%s/%s.rb", sp_feature_roots[ri], lib_name);
          content = read_file(rp);
          if (!content) {
            snprintf(rp, sizeof(rp), "%s/%s/%s.rb", sp_feature_roots[ri], lib_name, last);
            content = read_file(rp);
          }
          if (content) snprintf(lib_path, sizeof(lib_path), "%s", rp);
        }
        char *rc = content ? sp_canonical_path(lib_path) : NULL;
        for (int i = sp_rr_included; rc && i < sp_included_count && !root_dup; i++) root_dup = sp_included_paths[i] && strcmp(sp_included_paths[i], rc) == 0;
        if (root_dup) {
          free(content); content = strdup("# require skipped (already included)");
          dval = sp_defer_lookup(rc);
        }
        else if (rc) sp_mark_path_included(rc);
        free(rc);
      }
      if (!content) {
        /* pre-installed packages (the carved-out stdlib): packages/ sits
           beside lib/ in both the repo and the installed tree. The package
           root is the require root, so `require "erb"` is
           packages/erb/erb.rb and `require "erb/util"` is
           packages/erb/erb/util.rb. */
        char gp[1024];
        char first[256];
        const char *pkg = sp_package_feature(lib_name);
        {
          const char *sl = pkg ? strchr(pkg, '/') : NULL;
          size_t fl2 = !pkg ? 0 : sl ? (size_t)(sl - pkg) : strlen(pkg);
          if (fl2 >= sizeof(first)) fl2 = sizeof(first) - 1;
          if (pkg) memcpy(first, pkg, fl2);
          first[fl2] = 0;
        }
        int base_len = (int)strlen(lib_dir);
        if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
        /* builtins/: the core methods written in Ruby (builtins/enumerable.rb),
           spliced by sp_splice_builtins below rather than by a require the
           program writes; `require "builtins/<x>"` is <root>/builtins/<x>.rb */
        if (strncmp(lib_name, "builtins/", 9) == 0) {
          snprintf(gp, sizeof(gp), "%.*s/%s.rb", base_len, lib_dir, lib_name);
          content = read_file(gp);
          if (!content) {
            snprintf(gp, sizeof(gp), "%.*s/../%s.rb", base_len, lib_dir, lib_name);
            content = read_file(gp);
          }
          if (content) sp_note_builtin_path(gp);
        }
        if (!content && pkg) {
          snprintf(gp, sizeof(gp), "%.*s/packages/%s/%s.rb", base_len, lib_dir, first, pkg);
          content = read_file(gp);
        }
        if (!content && pkg) {
          /* dev binary one level below the repo root (build/spinel) */
          snprintf(gp, sizeof(gp), "%.*s/../packages/%s/%s.rb", base_len, lib_dir, first, pkg);
          content = read_file(gp);
        }
        /* the ffi package is glue over the system libffi, built only where
           that is installed: without its object the require stays the
           builtin DSL's (the tolerated no-op below) */
        if (content && strcmp(lib_name, "ffi") == 0) {
          char op[1200];
          snprintf(op, sizeof op, "%.*s", (int)(strlen(gp) - strlen("ffi.rb")), gp);
          strncat(op, "sp_ffi.o", sizeof op - strlen(op) - 1);
          FILE *of = fopen(op, "rb");
          if (of) fclose(of);
          else { free(content); content = NULL; }
        }
        /* fiddle is the ffi package's native layer under the stdlib's API: it
           exists where that object does */
        if (content && strcmp(lib_name, "fiddle") == 0) {
          char op[1200];
          snprintf(op, sizeof op, "%.*s", (int)(strlen(gp) - strlen("fiddle/fiddle.rb")), gp);
          strncat(op, "ffi/sp_ffi.o", sizeof op - strlen(op) - 1);
          FILE *of = fopen(op, "rb");
          if (of) fclose(of);
          else { free(content); content = NULL; }
        }
#if defined(_WIN32)
        /* what a native Windows build answers differently for this file:
           lib/win32/overlay/<the same path>, appended (sp_win32_driver.c) */
        if (content) content = sp_w32_overlay(content, gp);
#endif
        if (content) snprintf(lib_path, sizeof(lib_path), "%s", gp);
        /* --share-strings: a package's share declarations for it
           (`native_share`), in <name>.share.rb beside the file, follow it.
           Kept apart so a build without the flag parses no node of them. */
        if (content && sp_share_strings_env() && strlen(gp) > 3) {
          char sp[1100];
          snprintf(sp, sizeof sp, "%.*s.share.rb", (int)(strlen(gp) - 3), gp);
          char *side = read_file(sp);
          if (side) {
            size_t cl = strlen(content), sl = strlen(side);
            char *both = malloc(cl + sl + 2);
            if (!both) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
            memcpy(both, content, cl);
            both[cl] = '\n';
            memcpy(both + cl + 1, side, sl + 1);
            free(content); free(side);
            content = both;
          }
        }
      }
      if (!content) {
        if (sp_lib_is_native(lib_name)) {
          /* Provided natively by the Spinel runtime; the require is a
             harmless no-op, so don't warn. The require-gate still records it
             so the C-native feature (e.g. io/console) becomes available. */
          sp_feature_mark(lib_name);
          content = strdup("# require provided by Spinel runtime");
          req_val = "false";   /* already there: nothing was loaded */
          sp_req_resolved++;
        }
else if (sp_require_tolerated(lib_name)) {
          /* A core capability Spinel provides without a file (Thread,
             Enumerator, Fiber, the ffi DSL); the require is a harmless
             no-op, like modern CRuby -- gate or no gate. */
          content = strdup("# require no-op (core feature)");
          req_val = "false";   /* already there: nothing was loaded */
          sp_req_resolved++;
        }
else if (g_require_gate) {
          /* Whole-program AOT: an unsatisfiable require can never be provided, so
             it is a compile-time refusal (symmetric with the require_relative
             missing-file hard error), not a runtime LoadError. */
          fprintf(stderr,
                  "spinel: cannot load such file -- %s (require \"%s\")\n",
                  lib_name, lib_name);
          exit(1);
        }
else {
          fprintf(stderr,
                  "warning: '%s' is not available in Spinel; the require is ignored and code using it will fail\n",
                  lib_name);
          content = strdup("# require not resolved");
        }
      }
else {
        /* A bundled lib/<name>.rb was found and spliced; record the feature so
           the require-gate enables any C-native methods it stands in for. */
        sp_feature_mark(lib_name);
        req_val = root_dup ? (dval ? dval : "false") : "true";   /* this require is what loaded it */
        char *resolved = resolve_requires(content, lib_path, &cfsl, &cfsl_n);
        free(content);
        content = resolved;
        sp_req_resolved++;
      }
    }
    /* Stub arms above leave cfsl NULL: their content is a one-line comment. */
    if (!cfsl) cfsl = sp_fsl_make(content, 0, &cfsl_n);
    if (sp_require_preloaded(lib_name)) req_val = "false";
    if (strcmp(req_val, "nil") != 0) sp_note_required_feature(lib_name);

    /* Debug: marker-wrap so plain-require'd lib content doesn't corrupt the
       line map's accounting for code after the require. No-op without --debug. */
    content = sp_wrap_included(content, lib_path);
    sp_fsl_pad_wrap(&cfsl, &cfsl_n);

    /* The call's value is live: the require sits in a condition, an
       assignment, a receiver chain, or carries a trailing modifier
       (`require 'x' rescue nil`) that a comment in its place would strand.
       Inline the library ahead of the line and leave the value behind, so the
       statement around it survives and answers what CRuby answers (#3454). */
    {
      char *trail = expr_end;
      while (*trail == ' ' || *trail == '\t') trail++;
      /* `\r` is part of a CRLF line ending, not a modifier: treat it as a
         statement boundary so Windows sources keep the normal inlining path. */
      int has_modifier = (*trail != '\n' && *trail != '\r' && *trail != ';' &&
                          *trail != '\0' && *trail != '#');
      ml = sp_req_if_modifier(expr_end);
      cond_inline = !hit.margin || ml || sp_in_cond_region(result, hit.kw);
      /* a later require of a file a hoisted one deferred stands where it
         was, and runs what that one held back */
      if (dval) {
        free(content); content = strdup("");
        free(cfsl); cfsl = sp_fsl_make(content, 0, &cfsl_n);
      }
      if (dval || (!hit.margin && !sp_req_in_control_only(result, hit.kw)) || (has_modifier && !ml)) {
        if (!dval && strcmp(req_val, "true") == 0) {
          /* lib_path is the file found, which a later require of another
             name for it reaches (root_dup below) */
          char *found = sp_canonical_path(lib_path);
          const char *v = sp_defer_runtime(&content, canon_keep, found, incl_mark);
          free(found);
          if (v) req_val = v;
        }
        sp_req_hoist_splice(&result, fsl, fsl_n, &hit, content, cfsl, cfsl_n, req_val);
        free(cfsl);
        free(content);
        free(canon_keep);
        continue;
      }
    }
    free(canon_keep);

    /* a conditional inclusion is marked, so the requires inside it count as
       conditional too (the markers are comments: two lines, no flags) */
    if (cond_inline && (!hit.margin || ml)) content = sp_req_cond_wrap(content, &cfsl, &cfsl_n, expr_end, ml);

    /* Replace only the `require "name"` statement itself, not the whole
       line, so `require "x"; code` keeps `code`. Consume trailing
       horizontal whitespace and a single terminating newline (so a
       require on its own line leaves no blank line); stop at `;` or any
       other trailing code, which the inserted content's trailing newline
       then pushes onto its own line. */
    char *stmt_end = expr_end + ml;
    while (*stmt_end == ' ' || *stmt_end == '\t') stmt_end++;
    int consumed_nl = (*stmt_end == '\n');
    if (consumed_nl) stmt_end++;
    size_t line_len = stmt_end - pos;
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;

    /* Flag-buffer mirror: the require line is consumed entirely (its one
       entry replaced by the lib's entries) -- unless trailing code stayed on
       it, in which case that remainder becomes its own line and keeps the
       original line's entry (insert the lib's entries before it). At EOF
       with no trailing newline there is no remainder either. */
    sp_fsl_splice(fsl, fsl_n, sp_fsl_line_at(result, before_len),
                  (consumed_nl || *stmt_end == '\0') ? 1 : 0, cfsl, cfsl_n);
    free(cfsl);
    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);
    free(result);
    result = new_result;
    if (cond_inline)
      while (sp_included_count > incl_mark) free(sp_included_paths[--sp_included_count]);
    free(content);
  }
  return result;
}

/* ---- The names a constant holds, for def_delegators ---- */

/* The names of the constant NAME that the `def_delegators ... *NAME` at
   `before` splats, read from the source text. Its one definition in the
   file must be a line of its own at the call's (indented) indentation, in
   the same class body with no heredoc between, holding a literal Symbol
   or String Array (`[:a, :b]` or `["a", "b"]` over any lines, `%i[a b]` or
   `%w[a b]`) followed by nothing but
   `.freeze` and a comment; every other use of the name must be a splat or a
   read (`.each`, `.include?`, ...). 0 for anything else, which the caller
   refuses. */
static int sp_ident_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}
static size_t sp_line_start(const char *src, size_t p) {
  while (p > 0 && src[p - 1] != '\n') p--;
  return p;
}
static size_t sp_indent(const char *src, size_t ls) {
  size_t q = ls;
  while (src[q] == ' ' || src[q] == '\t') q++;
  return q - ls;
}
static int sp_const_symbol_list(const char *src, size_t len, size_t before, const char *name,
                                char (**out)[160], int *nout) {
  size_t nl = strlen(name);
  size_t call_ls = sp_line_start(src, before), call_ind = sp_indent(src, call_ls);
  /* a class body at the top level is not indented: too close to its
     neighbours to tell apart by indentation alone */
  if (call_ind == 0) return 0;
  /* Every use of the name in the file must be one of: its one definition
     (checked below), a `*NAME` splat, or a call of a method that only reads
     the Array. Anything else -- a second assignment (in a string, a branch,
     after `;`), an alias, `<<`, `A::NAME.push` -- could change what it
     holds, and refuses. */
  static const char *const reads[] = {
    ".each", ".each_with_index", ".include?", ".map", ".size", ".length", ".first",
    ".last", ".join", ".to_a", ".freeze", ".frozen?", ".dup", ".any?", ".all?", ".none?",
    ".count", ".index", ".sort", ".min", ".max", ".inspect", ".to_s", ".empty?", NULL };
  int ndefs = 0;
  for (size_t p = 0; p + nl <= len; p++) {
    if (strncmp(src + p, name, nl) != 0) continue;
    if ((p > 0 && sp_ident_char(src[p - 1])) || sp_ident_char(src[p + nl])) continue;
    if (p > 0 && src[p - 1] == '*') continue;                        /* a splat */
    size_t q = p + nl;
    while (q < len && (src[q] == ' ' || src[q] == '\t')) q++;
    if (src[q] == '=' && src[q + 1] != '=' && src[q + 1] != '~' && src[q + 1] != '>') { ndefs++; continue; }
    int ok = 0;
    for (int r = 0; reads[r] && !ok; r++) {
      size_t rl = strlen(reads[r]);
      if (strncmp(src + q, reads[r], rl) == 0 && !sp_ident_char(src[q + rl]) &&
          src[q + rl] != '!' && src[q + rl] != '=') ok = 1;
    }
    if (!ok) return 0;
  }
  if (ndefs != 1) return 0;
  /* the last definition line at the call's indentation, walking back */
  size_t at = (size_t)-1;
  size_t ls = call_ls;
  while (ls > 0) {
    ls = sp_line_start(src, ls - 1);
    size_t ind = sp_indent(src, ls), t = ls + ind;
    if (src[t] == '\n' || src[t] == '#' || src[t] == '\r') continue;   /* blank or comment */
    if (ind < call_ind) return 0;                                     /* left the class body */
    for (size_t h = t; src[h] != '\n'; h++)                           /* a heredoc opens here */
      if (src[h] == '<' && src[h + 1] == '<' && (src[h + 2] == '~' || src[h + 2] == '-' ||
          isupper((unsigned char)src[h + 2]))) return 0;
    if (ind != call_ind || strncmp(src + t, name, nl) != 0 || sp_ident_char(src[t + nl])) continue;
    size_t q = t + nl;
    while (src[q] == ' ' || src[q] == '\t') q++;
    if (src[q] != '=' || src[q + 1] == '=' || src[q + 1] == '~' || src[q + 1] == '>') continue;
    at = q + 1;
    break;
  }
  if (at == (size_t)-1) return 0;
  size_t q = at;
  while (q < before && (src[q] == ' ' || src[q] == '\t')) q++;
  int pct = 0;
  char close = ']';
  /* `%w[...]` and Strings name the same methods: Forwardable takes either */
  if (src[q] == '%' && (src[q + 1] == 'i' || src[q + 1] == 'w') && (src[q + 2] == '[' || src[q + 2] == '(')) {
    pct = 1; close = src[q + 2] == '[' ? ']' : ')'; q += 3;
  }
  else if (src[q] == '[') q++;
  else return 0;
  int cap = 16, n = 0;
  char (*names)[160] = malloc(sizeof(*names) * (size_t)cap);
  if (!names) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (;;) {
    while (q < before && (src[q] == ' ' || src[q] == '\t' || src[q] == '\n' || src[q] == '\r' ||
                          (!pct && src[q] == ','))) q++;
    if (q >= before) { free(names); return 0; }
    if (src[q] == close) { q++; break; }
    char quote = 0;
    if (!pct) {
      if (src[q] == '"' || src[q] == '\'') quote = src[q];
      else if (src[q] != ':') { free(names); return 0; }
      q++;
    }
    /* a name, with a trailing ? ! or = (closed?, write!, v=), or :<< --
       the one operator the rewrite is known to delegate right */
    size_t s0 = q;
    if (sp_ident_char(src[q])) {
      while (q < before && sp_ident_char(src[q])) q++;
      if (src[q] == '?' || src[q] == '!' || (src[q] == '=' && src[q + 1] != '>')) q++;
    }
    else if (src[q] == '<' && src[q + 1] == '<' && src[q + 2] != '<' && src[q + 2] != '=') q += 2;
    if (q == s0 || q - s0 >= 160 || (quote && src[q] != quote)) { free(names); return 0; }
    size_t e0 = q;
    if (quote) q++;
    if (n == cap) {
      cap *= 2;
      char (*nn)[160] = realloc(names, sizeof(*names) * (size_t)cap);
      if (!nn) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      names = nn;
    }
    memcpy(names[n], src + s0, e0 - s0);
    names[n][e0 - s0] = 0;
    n++;
  }
  /* only `.freeze` and a comment may follow */
  if (strncmp(src + q, ".freeze", 7) == 0) q += 7;
  while (src[q] == ' ' || src[q] == '\t' || src[q] == '\r') q++;
  if (src[q] != '\n' && src[q] != '#' && src[q] != 0) { free(names); return 0; }
  if (n == 0) { free(names); return 0; }
  *out = names; *nout = n;
  return 1;
}

/* ---- Main ---- */
/* ---- recognising an inline RBS comment (docs/inline-rbs.md) ---- */

/* RDoc's directives are spelled `:name:` and none of them is a valid RBS
 * annotation, so `#:nodoc:` and its kind are documentation, not types
 * (RDoc 7.0: rdoc/markup/pre_process.rb, rdoc/parser/ruby.rb). One written
 * without its closing colon (`def x=(v) #:nodoc`) is documentation too: as a
 * type it could only name a type alias, which inline annotations never pin. */
static int ir_rdoc_directive(const char *s, size_t n) {
    static const char *names[] = {
        "arg", "args", "attr", "attr_accessor", "attr_reader", "attr_writer",
        "call-seq", "category", "doc", "enddoc", "include", "main", "method",
        "nodoc", "notnew", "not_new", "not-new", "section", "singleton-method",
        "startdoc", "stopdoc", "title", "yield", "yields", NULL
    };
    /* s points just past "#:" */
    size_t i = 0;
    while (i < n && (s[i] == '-' || s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z'))) i++;
    if (i == 0 || (i < n && s[i] != ':' && s[i] != ' ' && s[i] != '\t')) return 0;
    for (int k = 0; names[k]; k++)
        if (strlen(names[k]) == i && memcmp(names[k], s, i) == 0) return 1;
    return 0;
}

/* What kind of line a comment is, for the block grouping below. */
typedef enum { CL_PROSE, CL_COLON, CL_RBS } ir_cline;

static ir_cline ir_classify(const char *s, size_t n) {
    if (n >= 2 && s[1] == ':') return ir_rdoc_directive(s + 2, n - 2) ? CL_PROSE : CL_COLON;
    size_t i = 1;
    if (i < n && s[i] == ' ') i++;
    if (n - i >= 4 && memcmp(s + i, "@rbs", 4) == 0 &&
        (n - i == 4 || !((s[i + 4] >= 'a' && s[i + 4] <= 'z') || (s[i + 4] >= 'A' && s[i + 4] <= 'Z') ||
                         (s[i + 4] >= '0' && s[i + 4] <= '9') || s[i + 4] == '_')))
        return CL_RBS;
    return CL_PROSE;
}

#ifdef SPINEL_INLINE_RBS
#include "rbs_map.h"
#include "inline_rbs.c"
#else
/* Built without the rbs C parser (no `make deps`): an annotation cannot be
   read, and is not applied -- which is said once, at the first one, rather
   than nothing at all. */
static int sp_inline_rbs_run(pm_parser_t *parser, pm_node_t *root, const char *source, size_t len) {
  (void)root; (void)len;
  if (!sp_inline_rbs_enabled) return 0;
  int previous_line = 0, previous_col = -1, block_leading = 1;
  for (const pm_comment_t *c = (const pm_comment_t *)parser->comment_list.head; c; c = (const pm_comment_t *)c->node.next) {
    const uint8_t *s = c->location.start;
    size_t n = (size_t)(c->location.end - s);
    if (c->type != PM_COMMENT_INLINE || sp_in_builtin(s)) continue;
    const uint8_t *line_start = s;
    while (line_start > (const uint8_t *)source && line_start[-1] != '\n') line_start--;
    int leading = 1;
    for (const uint8_t *p = line_start; p < s; p++)
      if (*p != ' ' && *p != '\t') { leading = 0; break; }
    int bl = pm_newline_list_line(&parser->newline_list, s, parser->start_line);
    int col = (int)(s - line_start);
    int continued = leading && bl == previous_line + 1 && col == previous_col;
    if (!continued) block_leading = leading;
    previous_line = bl;
    previous_col = col;
    /* A trailing block is recognized by its first comment only. Leading
       @rbs lines and trailing #[...] applications have distinct placements. */
    if (n < 2 || (continued && !block_leading)) continue;
    ir_cline kind = ir_classify((const char *)s, n);
    if (kind == CL_COLON || (block_leading && kind == CL_RBS) || (!block_leading && s[1] == '[')) {
      const char *file = g_source_file;
      int line = bl;
      if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n && sp_line_orig[bl] > 0) { file = sp_file_table[sp_line_file[bl]]; line = sp_line_orig[bl]; }
      fprintf(stderr, "spinel: %s:%d: warning: inline RBS comments are not applied: this spinel was "
                      "built without the rbs parser (run `make deps` and rebuild, or pass --no-inline-rbs)\n", file, line);
      return 0;
    }
  }
  return 0;
}
static void sp_inline_rbs_emit(const pm_node_t *node, int id) { (void)node; (void)id; }
static void sp_inline_rbs_done(void) {}
static void sp_inline_rbs_free(void) {}
#endif

/* Parse `source_file` into a new node table. `argv0` is the invoking
   program path (used to locate the stdlib for plain `require`s). Returns
   the table, or NULL on a read or parse error. This is the library copy
   (the in-process lib API; no standalone CLI main). */
static NodeTable *sp_parse_emit(const char *source_file, const char *argv0) {
  char *source = read_file(source_file);
  if (!source) {
    fprintf(stderr, "spinel_parse: cannot open '%s'\n", source_file);
    return NULL;
  }

  /* Set the debug flag before resolving requires so the resolvers insert the
     PUSH/POP markers used to rebuild the multi-file source map. */
  {
    const char *dbg = getenv("SPINEL_DEBUG");
    const char *lm = getenv("SPINEL_LINE_MAP");
    const char *ps = getenv("SPINEL_POSITIONS");
    int on = (dbg != NULL && dbg[0] == '1' && dbg[1] == '\0')
          || (lm  != NULL && lm[0]  == '1' && lm[1]  == '\0')
          || (ps  != NULL && ps[0]  == '1' && ps[1]  == '\0');
    g_emit_line = on ? 1 : 0;
    const char *et = getenv("SPINEL_EMIT_TYPES");
    const char *ww = getenv("SPINEL_WARN_WIDEN");
    const char *nc = getenv("SPINEL_NEW_CLAIMS");
    g_emit_end = (on && ((et && *et) || (ww && *ww) || (nc && *nc))) ? 1 : 0;
  }

  /* Resolve require_relative and plain require */
  /* Register the entry file itself as already-included, so a circular
     require_relative pointing back at it resolves to the dedup stub
     instead of splicing the entry's body a second time (#1373). */
  {
    char *entry_canon = sp_canonical_path(source_file);
    sp_mark_path_included(entry_canon);
    free(entry_canon);
  }
  /* `--require-gate` is the flag spelling of SPINEL_REQUIRE_GATE. An env
     assignment cannot ride inside a flag string, and `spin flags` has to hand
     a caller's Makefile the same gate `spin build` compiles under. */
  g_require_gate = (g_require_gate_cli || getenv("SPINEL_REQUIRE_GATE")) ? 1 : 0;
  unsigned char *fsl = NULL; size_t fsl_n = 0;
  sp_autoload_is_main = 1;
  char *resolved = resolve_requires(source, source_file, &fsl, &fsl_n);
  sp_rr_included = sp_included_count;
  free(source);
  source = resolve_plain_requires(resolved, argv0, &fsl, &fsl_n);
  source = sp_splice_named_builtin(source, argv0, "Gem", "builtins/gem", &fsl, &fsl_n);
  source = sp_splice_named_builtin(source, argv0, "RbConfig", "builtins/rbconfig", &fsl, &fsl_n);
  source = sp_splice_named_builtin(source, argv0, "ThreadGroup", "builtins/thread_group", &fsl, &fsl_n);
  source = sp_splice_monitor_mixin(source, argv0, &fsl, &fsl_n);
  source = sp_splice_object_space(source, argv0, &fsl, &fsl_n);
  source = sp_splice_process_detach(source, argv0, &fsl, &fsl_n);
  source = sp_splice_system_spawn(source, argv0, &fsl, &fsl_n);
  source = sp_splice_io_popen(source, argv0, &fsl, &fsl_n);
  source = sp_splice_foreach_first(source, argv0, &fsl, &fsl_n);
  /* CRuby provides Set (3.2+) and IO::Buffer without a require wherever
     they are used, in a required file as well (activesupport's
     notifications/fanout.rb; #6740 for IO::Buffer). Ask over the resolved
     program, the way the builtins splice below does, and splice the binding
     ahead of everything when it is named and was not required.
     sp_prepend_require marks the splice as inserted, so the program's own
     line numbers stay where they are; a require written into the entry
     file's text instead took a line of its own and pushed every later line
     down by one. */
  if (!sp_feature_required("set") && !sp_src_opens(source, "class Set") &&
      source_references_set(source))
    source = sp_prepend_require(source, argv0, "require \"set\"\n", &fsl, &fsl_n);
  if (!sp_feature_required("io/buffer") && source_references_io_buffer(source))
    source = sp_prepend_require(source, argv0, "require \"io/buffer\"\n", &fsl, &fsl_n);
  /* CRuby's time.rb (packages/time) for a program that requires "time" and
     calls Time.parse or Time.strptime; any other program keeps the
     runtime's C (#9946). */
  if (sp_feature_required("time") && source_calls_time_parser(source))
    source = sp_prepend_require(source, argv0, "require \"" SP_TIME_PARSERS_FEATURE "\"\n", &fsl, &fsl_n);
  source = sp_splice_builtins(source, argv0, &fsl, &fsl_n);
  source = sp_splice_builtin_extras(source, argv0, &fsl, &fsl_n);
  source = sp_splice_builtin_enumerator(source, argv0, &fsl, &fsl_n);
  sp_src_lex_drop();
  if (sp_req_written != sp_req_resolved) g_require_unread = 1;

  /* class-body loops over a literal Hash that define constants and classes
     (`H.each { |k, v| const_set(..., Class.new(P)) }`), run here and written
     back as the body's statements; line count kept */
  { char *tl = sp_expand_table_loops(source);
    if (tl) { free(source); source = tl; } }

  /* class-body macro calls (module_eval'd templates, computed
     attach_function / const_set names) expanded in place; line count kept */
  { char *mx = sp_expand_class_macros(source);
    if (mx) {
      /* what a macro wrote was in no file's text. A require it wrote is a
         call in the node table, where the analyzer finds it; a load-path
         statement is dropped ahead of the table, so g_require_unread reads
         the text here, past the count (the resolver's own stubs hold the
         word require) */
      sp_require_note_text(mx, strlen(mx));
      free(source); source = mx;
    } }

  /* Adopt the per-line pragma table only while it provably still lines up
     with the buffer (the expansions above keep the line count); on mismatch
     every literal gets the flag every file's literals get (sp_node_fsl). */
  if (sp_count_lines(source) == fsl_n) {
    g_fsl_lines = fsl;
    g_fsl_nlines = fsl_n;
  }
else {
    free(fsl);
    g_fsl_lines = NULL;
    g_fsl_nlines = 0;
  }
  /* the buffer-line -> (file, original line) map, from the buffer Prism
     parses: its marker lines are comments */
  if (g_src_map) sp_build_line_map(source, source_file);

  size_t source_len = strlen(source);
  sp_find_builtin_ranges(source);

  /* Parse with Prism */
  pm_parser_t parser;
  sp_pm_parser_init(&parser, source, source_len, NULL);
  pm_node_t *root = pm_parse(&parser);

  if (parser.error_list.size > 0) {
    /* Each error at its position, `file:line:col: message` (the column
       1-based on stderr, as an editor reads a compiler's message; 0-based in
       the JSON, as Prism gives it and --emit-types carries it), through the
       multi-file map when one was built so an error in a required file
       names that file. Under --emit-types the JSON is written too, with no
       types and the errors as its diagnostics: a buffer is unparseable most
       of the time it is being typed into, and its marker belongs on the
       token, not on line 1. */
    fprintf(stderr, "Parse errors in '%s':\n", source_file);
    const char *types_out = getenv("SPINEL_EMIT_TYPES");
    FILE *jf = (types_out && *types_out) ? fopen(types_out, "w") : NULL;
    if (types_out && *types_out && !jf) fprintf(stderr, "spinel: cannot write '%s'\n", types_out);
    if (jf) fputs("{\n  \"types\": [\n\n  ],\n  \"diagnostics\": [\n", jf);
    int nd = 0;
    pm_diagnostic_t *diag;
    for (diag = (pm_diagnostic_t *)parser.error_list.head; diag; diag = (pm_diagnostic_t *)diag->node.next) {
      pm_line_column_t lc = pm_newline_list_line_column(&parser.newline_list, diag->location.start, parser.start_line);
      pm_line_column_t le = pm_newline_list_line_column(&parser.newline_list, diag->location.end, parser.start_line);
      const char *file = source_file;
      int line = lc.line, end_line = le.line;
      if (sp_line_map_n > 0 && lc.line >= 1 && lc.line <= sp_line_map_n && sp_line_orig[lc.line] > 0) {
        file = sp_file_table[sp_line_file[lc.line]];
        line = sp_line_orig[lc.line];
        end_line = (le.line >= 1 && le.line <= sp_line_map_n && sp_line_orig[le.line] > 0) ? sp_line_orig[le.line] : line;
      }
      fprintf(stderr, "  %s:%d:%d: %s\n", file, line, lc.column + 1, diag->message);
      if (jf) {
        if (nd++) fputs(",\n", jf);
        fputs("    {\"file\":\"", jf);
        json_fputs(file, jf);
        fprintf(jf, "\",\"line\":%d,\"col\":%d,\"end_line\":%d,\"end_col\":%d,\"severity\":\"error\",\"message\":\"",
                line, lc.column, end_line, le.column);
        json_fputs(diag->message, jf);
        fputs("\"}", jf);
      }
    }
    if (jf) {
      fputs("\n  ],\n  \"codegen\": [\n\n  ]\n}\n", jf);
      fclose(jf);
      fprintf(stderr, "Wrote %s\n", types_out);
    }
    /* Issue #764: free the registered include paths on the parse-
       error exit path too. */
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(source);
    free(g_fsl_lines); g_fsl_lines = NULL; g_fsl_nlines = 0;
    sp_includes_free();
    return NULL;
  }

  g_parser = &parser;
  g_source_file = source_file;

  /* Inline RBS: the comment list and the tree are alive together only here. */
  if (sp_inline_rbs_run(&parser, root, source, source_len) > 0) {
    sp_inline_rbs_failed = 1;
    sp_inline_rbs_free();
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(source);
    free(g_fsl_lines); g_fsl_lines = NULL; g_fsl_nlines = 0;
    sp_includes_free();
    return NULL;
  }

  /* Flatten Prism's tree into the node table */
  out_nt = nt_new_table();
  node_counter = 0;
  out_nt->root_id = flatten(root);
  /* Issue #878: the source file path is a fact of the table, so `__dir__`
     and similar compile-time helpers can recover it even when the source
     contains no `__FILE__` reference. The id -> path table resolves each
     node's `node_file` id for its `#line` directive. */
  nt_put_source_file(out_nt, g_source_file);
  for (int i = 0; i < sp_file_count; i++) nt_put_file(out_nt, i, sp_file_table[i]);
  NodeTable *nt = out_nt;
  out_nt = NULL;

  sp_inline_rbs_done();
  sp_inline_rbs_free();
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  free(source);
  free(g_fsl_lines); g_fsl_lines = NULL; g_fsl_nlines = 0;
  sp_includes_free();
  return nt;
}

/* The library copy of the Prism front-end, linked into the C compiler
   (build/spinel): only the in-process lib API, no standalone CLI main. */

/* In-process entry for the single-binary C compiler: parse `source_file`
   into a node table (the caller frees it with nt_free), or NULL on error.
   `argv0` locates the stdlib for plain requires. */
NodeTable *sp_parse_file(const char *source_file, const char *argv0) {
  return sp_parse_emit(source_file, argv0);
}

/* Parse a Ruby snippet -- a class_eval string a desugar can read at compile
   time -- into a node table of its own, as the entry program is: no source
   path, no file map. NULL when it does not parse. The flattener's
   globals belong to the entry parse, which is over by the time this runs;
   they are saved around the call and reset the way that parse resets them.
   The entry's source map and per-line frozen_string_literal table describe
   the entry buffer, not this text, so they are off for it; a node's line is
   its line in the text (when the entry has lines at all), which the desugar
   moves to the eval site's. NULL as well for text that does not read the
   same spliced into the body (sp_snippet_graftable). */
NodeTable *sp_parse_snippet(const char *src) {
  if (!src || !sp_snippet_graftable(src)) return NULL;
  /* the entry buffer's state the flattener reads: its source map, per-line
     frozen_string_literal table, the statement cursor and owner */
  const pm_parser_t *saved_parser = g_parser;
  int saved_emit_line = g_emit_line, saved_emit_end = g_emit_end;
  NodeTable *saved_nt = out_nt; int saved_counter = node_counter;
  int saved_map_n = sp_line_map_n; unsigned char *saved_fsl = g_fsl_lines; size_t saved_fsl_n = g_fsl_nlines;
  pm_node_t **saved_next = g_stmt_next, **saved_end = g_stmt_end; const uint8_t *saved_owner = g_owner;
  sp_line_map_n = 0; g_fsl_lines = NULL; g_fsl_nlines = 0;
  g_stmt_next = g_stmt_end = NULL; g_owner = NULL; g_emit_end = 0;
  char *buf = strdup(src);
  NodeTable *res = NULL;
  if (buf) {
    pm_parser_t parser;
    sp_pm_parser_init(&parser, buf, strlen(buf), NULL);
    pm_node_t *root = pm_parse(&parser);
    if (parser.error_list.size == 0) {
      g_parser = &parser;
      out_nt = nt_new_table(); node_counter = 0;
      out_nt->root_id = flatten(root);
      res = out_nt;
    }
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(buf);
  }
  out_nt = saved_nt; node_counter = saved_counter;
  g_parser = saved_parser; g_emit_line = saved_emit_line; g_emit_end = saved_emit_end;
  sp_line_map_n = saved_map_n; g_fsl_lines = saved_fsl; g_fsl_nlines = saved_fsl_n;
  g_stmt_next = saved_next; g_stmt_end = saved_end; g_owner = saved_owner;
  return res;
}
