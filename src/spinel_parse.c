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
#include <sys/stat.h>    /* stat: a require's file is a regular one */
#include <dirent.h>      /* opendir: the builtins/ a graph program may load */
#include <ctype.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#include <prism.h>
#include "node_table.h"
#include "builtin_names.h"   /* the name layer: lowering rows, native classes, package facts */
#include "pre_const.h"       /* compile-time target platform, -D defines, constant-branch evaluator */

/* node_table.h names the node flags the passes read without including Prism;
   each must be the value Prism's own constant has. */
_Static_assert(NT_RANGE_EXCLUDE_END == PM_RANGE_FLAGS_EXCLUDE_END, "range exclude-end flag");
_Static_assert(NT_LOOP_BEGIN_MODIFIER == PM_LOOP_FLAGS_BEGIN_MODIFIER, "loop begin-modifier flag");
_Static_assert(NT_RE_IGNORE_CASE == PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE, "regexp /i flag");
_Static_assert(NT_RE_EXTENDED == PM_REGULAR_EXPRESSION_FLAGS_EXTENDED, "regexp /x flag");
_Static_assert(NT_RE_MULTI_LINE == PM_REGULAR_EXPRESSION_FLAGS_MULTI_LINE, "regexp /m flag");
_Static_assert(NT_RE_ONCE == PM_REGULAR_EXPRESSION_FLAGS_ONCE, "regexp /o flag");
_Static_assert(NT_RE_EUC_JP == PM_REGULAR_EXPRESSION_FLAGS_EUC_JP, "regexp /e flag");
_Static_assert(NT_RE_ASCII_8BIT == PM_REGULAR_EXPRESSION_FLAGS_ASCII_8BIT, "regexp /n flag");
_Static_assert(NT_RE_WINDOWS_31J == PM_REGULAR_EXPRESSION_FLAGS_WINDOWS_31J, "regexp /s flag");
_Static_assert(NT_RE_UTF_8 == PM_REGULAR_EXPRESSION_FLAGS_UTF_8, "regexp /u flag");

#if defined(_WIN32)
char *sp_w32_overlay(char *content, const char *path);   /* lib/win32/sp_win32_driver.c */
int sp_w32_package_absent(const char *pkg);               /* lib/win32/sp_win32_driver.c */
#endif

/* ---- Output ----
   The table the walk fills, and the id the next node it reaches gets. */
static NodeTable *out_nt;
static int node_counter;

/* ---- Name from constant pool ---- */
static const pm_parser_t *g_parser;
static const char *g_source_file = "";
/* Under g_opt_debug, g_opt_line_map or g_opt_positions, flatten() emits a
   per-node `node_line` field, which codegen places C `#line` directives by
   and the analysis reads. The compiler driver sets g_opt_positions for
   every compile; off otherwise, so the AST text format (and golden tests)
   are unchanged. */
static int g_emit_line = 0;
/* --emit-types (g_opt_emit_types) also gets each node's END position, so a
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
static void sp_pm_options_init(pm_options_t *opts, const char *path) {
  memset(opts, 0, sizeof *opts);
  pm_options_line_set(opts, 1);
  pm_options_frozen_string_literal_set(opts, true);
  pm_options_version_set(opts, "4.0", 3);
  if (path) pm_options_filepath_set(opts, path);
  /* the text of -e is parsed as ruby -e parses it, which differs only in
     the warnings: no unused local at the top level, and no literal in a
     condition taken for a comparison with $. */
  if (path && g_opt_eval_path && strcmp(path, g_opt_eval_path) == 0)
    pm_options_command_line_set(opts, PM_OPTIONS_COMMAND_LINE_E);
}
static void sp_pm_parser_init(pm_parser_t *parser, const char *src, size_t len,
                              const char *path) {
  pm_options_t opts;
  sp_pm_options_init(&opts, path);
  pm_parser_init(parser, (const uint8_t *)src, len, &opts);
  pm_options_free(&opts);
}

/* One file's text parsed by itself, before anything is spliced into it: the
   facts of the file (its frozen_string_literal pragma, its source encoding,
   the requires its dead branches hold) are read from this parse. */
typedef struct {
  const char *path;
  const char *shown;   /* the path a message names, when not path (sp_shown_path) */
  const char *text;
  pm_parser_t parser;
  pm_node_t *root;
} SpSrcFile;

/* The name a message gives file `path` (the whole path the graph holds),
   a file of the program's own (a compiler's library keeps the path it was
   found at): from the current directory when the entry file is given
   relative to it and the file is under it, else the path. A pointer into
   path. */
static const char *sp_shown_path(const char *path, const char *entry) {
  char cwd[1024];
  size_t cl = entry && entry[0] != '/' && getcwd(cwd, sizeof cwd) ? strlen(cwd) : 0;
  return cl && strncmp(path, cwd, cl) == 0 && path[cl] == '/' ? path + cl + 1 : path;
}

static void sp_src_file_parse(SpSrcFile *f, const char *text, const char *path) {
  f->path = path;
  f->shown = NULL;
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

/* The file's `# spinel: defer-refusals` magic comment: the file's refusals
   compile to raises of NotImplementedError where they stand, as
   --defer-refusals makes every file's (defer_refusals_at). A vendored
   library carries it where a feature Spinel does not compile (sqlite3's
   create_aggregate builds a class per call) must not keep a program that
   never calls it from building. The paths are kept until the node table
   takes its file table (sp_defer_paths_mark). */
static char **sp_defer_paths; static int sp_defer_npaths;
static char **sp_file_table; static int sp_file_count;   /* (defined with the map builder below) */
/* A file id the table holds twice under one path (sp_intern_display_file)
   says which of the two defers: 0 = its path decides, 1 = no, 2 = yes. */
static unsigned char *sp_file_defer_force;
static bool sp_magic_comment_is(const pm_magic_comment_t *mc, const char *key, const char *value) {
  if (mc->key_length != strlen(key) || mc->value_length != strlen(value)) return false;
  for (uint32_t i = 0; i < mc->key_length; i++) {
    char ch = (char)tolower(mc->key_start[i]);
    if ((ch == '-' ? '_' : ch) != key[i]) return false;
  }
  for (uint32_t i = 0; i < mc->value_length; i++) {
    char ch = (char)tolower(mc->value_start[i]);
    if ((ch == '_' ? '-' : ch) != value[i]) return false;
  }
  return true;
}
static void sp_src_file_note_defers(const SpSrcFile *f) {
  for (const pm_list_node_t *n = f->parser.magic_comment_list.head; n; n = n->next) {
    if (!sp_magic_comment_is((const pm_magic_comment_t *)n, "spinel", "defer-refusals")) continue;
    char **grown = realloc(sp_defer_paths, sizeof(char *) * (size_t)(sp_defer_npaths + 1));
    if (!grown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    sp_defer_paths = grown;
    sp_defer_paths[sp_defer_npaths++] = strdup(f->shown ? f->shown : f->path);   /* (the file table's name for it) */
    return;
  }
}
static int sp_path_defers(const char *path) {
  for (int k = 0; k < sp_defer_npaths; k++)
    if (path && strcmp(path, sp_defer_paths[k]) == 0) return 1;
  return 0;
}
/* Does file id i defer its refusals: as its own path says, unless the id was
   made for a marker's name (sp_intern_display_file) and says it itself. */
static int sp_file_id_defers(int i) {
  if (sp_file_defer_force && sp_file_defer_force[i]) return sp_file_defer_force[i] == 2;
  return sp_path_defers(sp_file_table[i]);
}
static void sp_defer_paths_mark(NodeTable *nt) {
  for (int i = 0; i < sp_file_count; i++)
    if (sp_file_id_defers(i)) nt_put_file_defers(nt, i);
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
          "ignored\n", f->shown ? f->shown : f->path, (int)line);
  return 1;
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
          "but take UTF-8 as their encoding\n", f->shown ? f->shown : f->path, (int)line, f->parser.encoding->name);
}

/* Debug multi-file source map, populated by sp_build_line_map() before
   flatten() runs and read in flatten() to attribute each node to its
   original file/line. Declared here because flatten() precedes the
   require-resolution machinery that defines the builder. */
static int *sp_line_file = NULL;  /* buffer line (1-based) -> file id */
static int *sp_line_orig = NULL;  /* buffer line (1-based) -> original line */
/* The position a `#<SPINEL_SOURCE>file:line` marker pins for the lines after
   it (0 = none). Kept apart from the physical map above: `__FILE__`,
   `__dir__` and `require_relative` still answer from the file the code is in,
   only the positions handed to `#line`, debug and the reports follow it. */
static int *sp_disp_file = NULL;
static int *sp_disp_line = NULL;
static const uint8_t *g_owner;
static int sp_line_map_n = 0;
static char **sp_file_table;   /* id -> path; defined with the map builder below */

/* The file being flattened, as the table's file list has it (0 is the
   entry); set with g_parser for each file. A require is a LoadNode
   (sp_flatten_call_as, below). */
static int g_flat_file = 0;
static int g_flat_builtin = 0;   /* ... and it is a builtins/ file */

/* The file a node was written in when that is a required file, or NULL for
   the entry script (file id 0). Each file is flattened from its own parse,
   which says which file it is. */
static const char *sp_node_required_file(const pm_node_t *node) {
  (void)node;
  return g_flat_file > 0 ? sp_file_table[g_flat_file] : NULL;
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

static int sp_in_builtin(const uint8_t *at);
#include "prism_eval.c"
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
static void sp_lower_rows_note(const pm_node_t *node, int id);   /* SPINEL_LOWER_ROWS_CHECK */
static int sp_load_call(const pm_call_node_t *n);
static int sp_pm_name_is(const pm_parser_t *parser, pm_constant_id_t id, const char *word);
static int sp_name_buf(const pm_parser_t *pp, pm_constant_id_t id, char *buf, size_t cap);
static int sp_flatten_call_as(int id, pm_call_node_t *n);
static int sp_graph_site_take(int id, const pm_node_t *call);   /* 1: its path is computed when it runs */
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

/* ---- The scope a local belongs to ----
   Prism pushes a scope at a program, class, module, singleton class, def,
   block and lambda; `for`, BEGIN/END and rescue push none, and a local's
   `depth` counts the scopes that way. The ids of the scopes being flattened,
   outermost first, make a local node's `depth` a node: its "decl" is the
   scope that declares it (stack[top - depth]), and a parameter's or block-
   local's is the innermost. A snippet's host scopes stand as -1 (a local
   there has no decl). */
/* An `it` block's parameters, lowered to one numbered parameter that Proc#parameters names no more than CRuby does. */
static void emit_it_parameters(int id) {
  nt_put_type(out_nt, id, "NumberedParametersNode");
  emit_int(id, "maximum", 1);
  emit_int(id, "implicit_it", 1);
}
static void decl_push(int id) { nt_build_scope_push(out_nt, id); }
static void decl_pop(void) { nt_build_scope_pop(out_nt); }
/* Node id is a local variable, parameter or block-local the program wrote as `w` in the scope `depth` out of the
   innermost: its name as written, and its decl (comp_name_locals gives a local of a scope that is no frame root its
   slot name). */
static void emit_local_s(int id, const char *w, int depth) {
  int d = nt_build_scope_at(out_nt, depth);
  emit_str(id, "name", w);
  if (d >= 0) emit_int(id, "decl", d);
}
static void emit_local(int id, pm_constant_id_t name, int depth) {
  char *w = cstr(name);
  emit_local_s(id, w, depth);
  free(w);
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

/* Field `field` of scope node id, flattened inside the scope. */
static void flat_scoped(int id, const char *field, pm_node_t *child) {
  decl_push(id);
  emit_ref(id, field, child);
  decl_pop();
}

/* `class A < (Base)`: parentheses around a single expression carry no
   meaning, but every pass reads the superclass by its node kind, and a
   ParenthesesNode read as no superclass at all (Object). The superclass
   without them. */
static pm_node_t *flat_superclass(pm_node_t *sup) {
  while (sup && PM_NODE_TYPE(sup) == PM_PARENTHESES_NODE) {
    pm_node_t *pb = ((pm_parentheses_node_t *)sup)->body;
    if (!pb || PM_NODE_TYPE(pb) != PM_STATEMENTS_NODE ||
        ((pm_statements_node_t *)pb)->body.size != 1) break;
    sup = ((pm_statements_node_t *)pb)->body.nodes[0];
  }
  return sup;
}

/* `in [Integer => a, (Integer => b)]`: parentheses around a sub-pattern
   group it and mean nothing more, but each pass that reads a pattern (the
   matcher, the bindings, their types) asks for its node kind. The pattern
   is rewritten in place without them: each ParenthesesNode that holds one
   pattern gives way to that pattern, at the root (also under an arm's
   guard) and at every sub-pattern slot. A pinned expression `^(expr)`
   keeps its own parentheses. */
static pm_node_t *flat_pattern(pm_node_t *pat);
static void flat_pattern_list(pm_node_list_t *list) {
  for (size_t i = 0; i < list->size; i++) list->nodes[i] = flat_pattern(list->nodes[i]);
}
static pm_node_t *flat_pattern(pm_node_t *pat) {
  while (pat && PM_NODE_TYPE(pat) == PM_PARENTHESES_NODE) {
    pm_parentheses_node_t *pn = (pm_parentheses_node_t *)pat;
    pm_node_t *pb = pn->body;
    if (!pb) break;
    if (PM_NODE_TYPE(pb) == PM_STATEMENTS_NODE) {
      pm_statements_node_t *st = (pm_statements_node_t *)pb;
      if (st->body.size != 1) break;
      pb = st->body.nodes[0];
      st->body.size = 0;   /* the shell goes, the pattern stays */
    }
    else pn->body = NULL;
    pm_node_destroy((pm_parser_t *)g_parser, pat);
    pat = pb;
  }
  if (!pat) return pat;
  switch (PM_NODE_TYPE(pat)) {
  case PM_ALTERNATION_PATTERN_NODE: {
    pm_alternation_pattern_node_t *n = (pm_alternation_pattern_node_t *)pat;
    n->left = flat_pattern(n->left);
    n->right = flat_pattern(n->right);
    break;
  }
  case PM_ARRAY_PATTERN_NODE: {
    pm_array_pattern_node_t *n = (pm_array_pattern_node_t *)pat;
    flat_pattern_list(&n->requireds);
    flat_pattern_list(&n->posts);
    break;
  }
  case PM_FIND_PATTERN_NODE:
    flat_pattern_list(&((pm_find_pattern_node_t *)pat)->requireds);
    break;
  case PM_HASH_PATTERN_NODE: {
    pm_hash_pattern_node_t *n = (pm_hash_pattern_node_t *)pat;
    for (size_t i = 0; i < n->elements.size; i++) {
      pm_node_t *e = n->elements.nodes[i];
      if (e && PM_NODE_TYPE(e) == PM_ASSOC_NODE) {
        pm_assoc_node_t *a = (pm_assoc_node_t *)e;
        a->value = flat_pattern(a->value);
      }
    }
    break;
  }
  case PM_CAPTURE_PATTERN_NODE: {
    pm_capture_pattern_node_t *n = (pm_capture_pattern_node_t *)pat;
    n->value = flat_pattern(n->value);
    break;
  }
  case PM_IF_NODE: case PM_UNLESS_NODE: {
    /* `in pat if guard`: the arm's pattern is the guard's one statement */
    pm_statements_node_t *st = PM_NODE_TYPE(pat) == PM_IF_NODE ? ((pm_if_node_t *)pat)->statements
                                                                : ((pm_unless_node_t *)pat)->statements;
    if (st && st->body.size == 1) st->body.nodes[0] = flat_pattern(st->body.nodes[0]);
    break;
  }
  default: break;
  }
  return pat;
}

/* A scope's own locals, joined with commas. A block's or lambda's (params +
   first-assigned-inside): Ruby scoping makes the non-param ones FRESH on
   every call, and a name the enclosing scope assigns only after the block's
   text is the block's own, not the enclosing one. A class or module body's:
   the frame an eval's text in it reads (sp_parse_snippet_in), as a def's
   is (a hook's, whose text runs where the hook is spliced). */
static void out_scope_locals(int id, const pm_constant_id_list_t *locals) {
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

/* A def's fields: its name, parameters, body and receiver, and its locals */
static void flat_def(int id, const pm_def_node_t *n) {
  char *nm = cstr(n->name);
  emit_str(id, "name", nm);
  free(nm);
  decl_push(id);
  emit_ref(id, "parameters", (pm_node_t *)n->parameters);
  emit_ref(id, "body", (pm_node_t *)n->body);
  decl_pop();
  emit_ref(id, "receiver", (pm_node_t *)n->receiver);
  out_scope_locals(id, &n->locals);
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
/* The node a block argument's proc is read from: `:name.to_proc`'s Symbol,
   as the proc is the one `&:name` passes, and any other node itself. */
static const pm_node_t *sp_sym_proc_source(const pm_node_t *e) {
  if (!e || PM_NODE_TYPE(e) != PM_CALL_NODE) return e;
  const pm_call_node_t *c = (const pm_call_node_t *)e;
  char name[8];
  if (!c->receiver || PM_NODE_TYPE(c->receiver) != PM_SYMBOL_NODE || c->arguments || c->block ||
      !sp_name_buf(g_parser, c->name, name, sizeof name) || !is_to_proc_name(name)) return e;
  return c->receiver;
}
/* The name of the Symbol block argument `node` flatten makes a block of
   (its length, the bytes in *s); 0 for any other node */
static size_t sp_sym_proc_name(const pm_node_t *node, const char **s) {
  if (PM_NODE_TYPE(node) != PM_BLOCK_ARGUMENT_NODE) return 0;
  /* the Symbol written in parentheses (`&(:to_s)`), quoted (`&:"to_s"`)
     or as its proc (`&:to_s.to_proc`) is the same block: left a
     BlockArgumentNode, a Symbol took no block */
  const pm_node_t *e = ((const pm_block_argument_node_t *)node)->expression;
  while (e && PM_NODE_TYPE(e) == PM_PARENTHESES_NODE) {
    const pm_node_t *pb = ((const pm_parentheses_node_t *)e)->body;
    if (!pb || PM_NODE_TYPE(pb) != PM_STATEMENTS_NODE || ((const pm_statements_node_t *)pb)->body.size != 1) { e = NULL; break; }
    e = ((const pm_statements_node_t *)pb)->body.nodes[0];
  }
  e = sp_sym_proc_source(e);
  size_t n = sp_plain_sym(e, s);
  if (!n) n = sp_quoted_recv_sym(e, s);
  int word = n > 0;
  for (size_t i = 0; i < n; i++)
    if (!sp_word_char((*s)[i]) && (*s)[i] != '?' && (*s)[i] != '!' && (unsigned char)(*s)[i] < 0x80) word = 0;
  if (!word && !(n == 2 && ((*s)[0] == '-' || (*s)[0] == '+') && (*s)[1] == '@') && !(n == 1 && (*s)[0] == '~'))
    return 0;
  return n;
}
static pm_node_t *sp_sym_proc_block(pm_node_t *node) {
  const char *s;
  size_t n = sp_sym_proc_name(node, &s);
  if (!n) return NULL;
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
   from (`node_bi`, comp_node_tag): its enclosing def, or, outside every def,
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
  sp_lower_rows_note(node, id);
  if (made) pm_node_destroy((pm_parser_t *)g_parser, made);
  g_bi_base = saved;
  return id;
}

/* A StatementsNode's body; a def_delegators statement is flattened as the
   defs it stands for. */
static void flatten_statements(int id, pm_node_list_t *list) {
  pm_node_list_t low = { 0 }, made = { 0 };
  pm_node_list_t *body = sp_lower_body(list, &low, &made) ? &low : list;
  emit_node_array(id, "body", body);
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
  if (name) emit_local(id, name, 0);
  if (value) emit_ref(id, "value", value);
}

/* An attribute's `+=`, `||=` and `&&=` calls the reader and then the writer:
   both names, the reader as `name` and the writer as `write_name`. */
static void emit_attr_names(int id, pm_constant_id_t read, pm_constant_id_t write) {
  char *r = cstr(read), *w = cstr(write);
  emit_str(id, "name", r);
  emit_str(id, "write_name", w);
  free(r);
  free(w);
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
/* ---- Pre-analysis constant-branch evaluation (Prism AST) ----
 * Shared with analysis via pre_const.c ("One place per decision"). Runs before
 * require resolution, boot-library name scanning, and flattening so untaken
 * RUBY_PLATFORM / RUBY_ENGINE / target-fixed defined? / -D branches never load
 * missing libraries, widen inference, or pollute g_require_unread, while
 * preserving Ruby's lexical local variable declarations (`dead_locals`). */

typedef struct {
  int initialized;
  int allow_engine;
  int allow_platform;
  int allow_version;
  int allow_object;
  int has_remove_const;
  int core_ops_reopened;
  int cli_defined_only;
  unsigned char *def_shadowed;
  int ndefs;
} SpPreFileCtx;

static const SpPreFileCtx *g_flat_pre = NULL;
static int g_in_pattern = 0;
static int g_in_basic_object = 0;

static void sp_pre_ctx_free(SpPreFileCtx *c) {
  if (!c) return;
  free(c->def_shadowed);
  memset(c, 0, sizeof *c);
}

/* Merge `src` into `dst`. Returns 1 if `dst` was already initialized and any
   permissive flag became more restrictive (cross-file shadowing / reopening). */
static int sp_pre_ctx_merge(SpPreFileCtx *dst, const SpPreFileCtx *src) {
  if (!dst || !src) return 0;
  if (!dst->initialized) {
    dst->initialized = 1;
    dst->allow_engine = src->allow_engine;
    dst->allow_platform = src->allow_platform;
    dst->allow_version = src->allow_version;
    dst->allow_object = src->allow_object;
    dst->has_remove_const = src->has_remove_const;
    dst->core_ops_reopened = src->core_ops_reopened;
    dst->cli_defined_only = 0;
    dst->ndefs = src->ndefs;
    if (src->ndefs > 0) {
      dst->def_shadowed = calloc((size_t)src->ndefs, 1);
      if (!dst->def_shadowed) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      if (src->def_shadowed) memcpy(dst->def_shadowed, src->def_shadowed, (size_t)src->ndefs);
    }
    return 0;
  }
  int tightened = 0;
  if (!src->allow_engine && dst->allow_engine) { dst->allow_engine = 0; tightened = 1; }
  if (!src->allow_platform && dst->allow_platform) { dst->allow_platform = 0; tightened = 1; }
  if (!src->allow_version && dst->allow_version) { dst->allow_version = 0; tightened = 1; }
  if (!src->allow_object && dst->allow_object) { dst->allow_object = 0; tightened = 1; }
  if (src->has_remove_const && !dst->has_remove_const) { dst->has_remove_const = 1; tightened = 1; }
  if (src->core_ops_reopened && !dst->core_ops_reopened) { dst->core_ops_reopened = 1; tightened = 1; }
  for (int i = 0; i < dst->ndefs && i < src->ndefs; i++)
    if (src->def_shadowed && src->def_shadowed[i] && !dst->def_shadowed[i]) {
      dst->def_shadowed[i] = 1;
      tightened = 1;
    }
  return tightened;
}

static int sp_pre_pm_id_is(const pm_parser_t *p, pm_constant_id_t id, const char *word) {
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&p->constant_pool, id);
  size_t n = strlen(word);
  return nm->length == n && memcmp(nm->start, word, n) == 0;
}

static int sp_pre_pm_const_name(const pm_parser_t *p, const pm_node_t *node,
                                const char **out, size_t *len, int allow_bare) {
  if (!node) return 0;
  pm_constant_id_t cid = 0;
  if (PM_NODE_TYPE(node) == PM_CONSTANT_READ_NODE) {
    if (!allow_bare) return 0;
    cid = ((const pm_constant_read_node_t *)node)->name;
  }
  else if (PM_NODE_TYPE(node) == PM_CONSTANT_PATH_NODE) {
    const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)node;
    if (cp->parent) {
      const char *pn = NULL; size_t pl = 0;
      if (!sp_pre_pm_const_name(p, cp->parent, &pn, &pl, allow_bare)) return 0;
      if (pl != 6 || memcmp(pn, "Object", 6) != 0) return 0;
    }
    cid = cp->name;
  }
  else return 0;
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&p->constant_pool, cid);
  *out = (const char *)nm->start;
  *len = nm->length;
  return 1;
}

static int sp_pre_pm_is_basic_object(const pm_parser_t *p, const pm_node_t *node) {
  const char *nm = NULL; size_t len = 0;
  return sp_pre_pm_const_name(p, node, &nm, &len, 1) && len == 11 && memcmp(nm, "BasicObject", 11) == 0;
}

static int sp_pre_pm_is_core_op_owner(const pm_parser_t *p, const pm_node_t *cpath) {
  const char *nm = NULL; size_t len = 0;
  return sp_pre_pm_const_name(p, cpath, &nm, &len, 1) && sp_pre_is_core_op_owner(nm, len);
}

typedef struct { const pm_parser_t *p; int hit; } SpPreOpScan;

static bool sp_pre_pm_op_visit(const pm_node_t *node, void *data) {
  SpPreOpScan *s = data;
  if (s->hit) return false;
  pm_node_type_t t = PM_NODE_TYPE(node);
  if (t == PM_CLASS_NODE || t == PM_MODULE_NODE || t == PM_SINGLETON_CLASS_NODE) return false;
  if (t == PM_DEF_NODE) {
    const pm_def_node_t *d = (const pm_def_node_t *)node;
    if (!d->receiver) {
      const pm_constant_t *nm = pm_constant_pool_id_to_constant(&s->p->constant_pool, d->name);
      char buf[32];
      if (nm->length < sizeof buf) {
        memcpy(buf, nm->start, nm->length); buf[nm->length] = '\0';
        if (sp_pre_is_folded_op(buf)) { s->hit = 1; return false; }
      }
    }
    return false;
  }
  if (t == PM_ALIAS_METHOD_NODE) {
    const pm_node_t *nn = ((const pm_alias_method_node_t *)node)->new_name;
    if (nn && PM_NODE_TYPE(nn) == PM_SYMBOL_NODE) {
      const pm_string_t *u = &((const pm_symbol_node_t *)nn)->unescaped;
      char buf[32];
      if (pm_string_length(u) < sizeof buf) {
        memcpy(buf, pm_string_source(u), pm_string_length(u)); buf[pm_string_length(u)] = '\0';
        if (sp_pre_is_folded_op(buf)) { s->hit = 1; return false; }
      }
    }
    return false;
  }
  if (t == PM_UNDEF_NODE) {
    const pm_node_list_t *nl = &((const pm_undef_node_t *)node)->names;
    for (size_t i = 0; i < nl->size; i++) {
      if (PM_NODE_TYPE(nl->nodes[i]) == PM_SYMBOL_NODE) {
        const pm_string_t *u = &((const pm_symbol_node_t *)nl->nodes[i])->unescaped;
        char buf[32];
        if (pm_string_length(u) < sizeof buf) {
          memcpy(buf, pm_string_source(u), pm_string_length(u)); buf[pm_string_length(u)] = '\0';
          if (sp_pre_is_folded_op(buf)) { s->hit = 1; return false; }
        }
      }
    }
    return false;
  }
  if (t == PM_CALL_NODE) {
    const pm_call_node_t *c = (const pm_call_node_t *)node;
    if (!c->receiver && (sp_pre_pm_id_is(s->p, c->name, "define_method") ||
                         sp_pre_pm_id_is(s->p, c->name, "alias_method") ||
                         sp_pre_pm_id_is(s->p, c->name, "undef_method") ||
                         sp_pre_pm_id_is(s->p, c->name, "remove_method") ||
                         sp_pre_pm_id_is(s->p, c->name, "include") ||
                         sp_pre_pm_id_is(s->p, c->name, "prepend"))) {
      s->hit = 1;
      return false;
    }
  }
  return true;
}

static int sp_pre_pm_eval(const pm_parser_t *p, const pm_node_t *node, const SpPreFileCtx *ctx,
                          int allow_bare, SpPreVal *out, int *used_const, int depth) {
  if (!node || !ctx || depth > 64) return 0;
  switch (PM_NODE_TYPE(node)) {
    case PM_TRUE_NODE: *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = 1 }; return 1;
    case PM_FALSE_NODE: *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = 0 }; return 1;
    case PM_NIL_NODE: *out = (SpPreVal){ .kind = SP_PRE_NIL }; return 1;
    case PM_INTEGER_NODE: {
      pm_integer_t *iv = &((pm_integer_node_t *)node)->value;
      if (pm_int_overflows(iv)) return 0;
      *out = (SpPreVal){ .kind = SP_PRE_INT, .i = pm_int_value(iv) };
      return 1;
    }
    case PM_STRING_NODE: {
      const pm_string_t *u = &((const pm_string_node_t *)node)->unescaped;
      *out = (SpPreVal){ .kind = SP_PRE_STR, .s = (const char *)pm_string_source(u), .slen = pm_string_length(u) };
      return 1;
    }
    case PM_PARENTHESES_NODE: {
      const pm_node_t *body = ((const pm_parentheses_node_t *)node)->body;
      if (!body) { *out = (SpPreVal){ .kind = SP_PRE_NIL }; return 1; }
      return sp_pre_pm_eval(p, body, ctx, allow_bare, out, used_const, depth + 1);
    }
    case PM_STATEMENTS_NODE: {
      const pm_node_list_t *b = &((const pm_statements_node_t *)node)->body;
      return b->size == 1 ? sp_pre_pm_eval(p, b->nodes[0], ctx, allow_bare, out, used_const, depth + 1) : 0;
    }
    case PM_CONSTANT_READ_NODE: case PM_CONSTANT_PATH_NODE: {
      const char *nm = NULL; size_t len = 0;
      if (sp_pre_pm_const_name(p, node, &nm, &len, allow_bare) &&
          sp_pre_lookup_const(nm, len, ctx->allow_engine, ctx->allow_platform,
                              ctx->allow_version, ctx->def_shadowed, out)) {
        *used_const = 1;
        return 1;
      }
      return 0;
    }
    case PM_DEFINED_NODE: {
      const char *nm = NULL; size_t len = 0;
      if (sp_pre_pm_const_name(p, ((const pm_defined_node_t *)node)->value, &nm, &len, allow_bare) &&
          (!ctx->cli_defined_only || sp_pre_define_index(nm, len) >= 0) &&
          sp_pre_defined_const_fixed(nm, len, ctx->has_remove_const) == 1) {
        *used_const = 1;
        *out = (SpPreVal){ .kind = SP_PRE_STR, .s = "constant", .slen = 8 };
        return 1;
      }
      return 0;
    }
    case PM_AND_NODE: case PM_OR_NODE: {
      int is_and = PM_NODE_TYPE(node) == PM_AND_NODE;
      const pm_node_t *l = is_and ? ((const pm_and_node_t *)node)->left : ((const pm_or_node_t *)node)->left;
      const pm_node_t *r = is_and ? ((const pm_and_node_t *)node)->right : ((const pm_or_node_t *)node)->right;
      int lc = 0; SpPreVal lv;
      if (!sp_pre_pm_eval(p, l, ctx, allow_bare, &lv, &lc, depth + 1)) return 0;
      int lt = sp_pre_val_truthy(&lv);
      if ((is_and && !lt) || (!is_and && lt)) {
        *used_const |= lc;
        *out = lv;
        return 1;
      }
      int rc = 0; SpPreVal rv;
      if (!sp_pre_pm_eval(p, r, ctx, allow_bare, &rv, &rc, depth + 1)) return 0;
      *used_const |= (lc | rc);
      *out = rv;
      return 1;
    }
    case PM_CALL_NODE: {
      const pm_call_node_t *c = (const pm_call_node_t *)node;
      if (c->block || !c->receiver || ctx->core_ops_reopened) return 0;
      int snav = PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION);
      const pm_constant_t *nm = pm_constant_pool_id_to_constant(&p->constant_pool, c->name);
      char op[32];
      if (nm->length >= sizeof op) return 0;
      memcpy(op, nm->start, nm->length);
      op[nm->length] = '\0';
      size_t ac = c->arguments ? c->arguments->arguments.size : 0;
      pm_node_t **av = c->arguments ? c->arguments->arguments.nodes : NULL;
      if (!snav && ctx->allow_object && strcmp(op, "const_defined?") == 0 && ac == 1) {
        const char *rn = NULL; size_t rl = 0;
        const pm_string_t *arg_s = PM_NODE_TYPE(av[0]) == PM_SYMBOL_NODE ? &((const pm_symbol_node_t *)av[0])->unescaped :
                                   PM_NODE_TYPE(av[0]) == PM_STRING_NODE ? &((const pm_string_node_t *)av[0])->unescaped : NULL;
        if (arg_s && sp_pre_pm_const_name(p, c->receiver, &rn, &rl, allow_bare) && rl == 6 && memcmp(rn, "Object", 6) == 0 &&
            (!ctx->cli_defined_only || sp_pre_define_index((const char *)pm_string_source(arg_s), pm_string_length(arg_s)) >= 0) &&
            sp_pre_defined_const_fixed((const char *)pm_string_source(arg_s), pm_string_length(arg_s), ctx->has_remove_const) == 1) {
          *used_const = 1;
          *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = 1 };
          return 1;
        }
        return 0;
      }
      int rc = 0; SpPreVal rv;
      if (!sp_pre_pm_eval(p, c->receiver, ctx, allow_bare, &rv, &rc, depth + 1)) return 0;
      if (snav && rv.kind == SP_PRE_NIL) return 0;
      if (ac == 0) {
        if (!sp_pre_eval_unary(&rv, op, out)) return 0;
        *used_const |= rc;
        return 1;
      }
      if (strcmp(op, "start_with?") == 0 || strcmp(op, "end_with?") == 0) {
        if (ac > 16) return 0;
        SpPreVal avals[16];
        int auc = rc;
        for (size_t i = 0; i < ac; i++)
          if (!sp_pre_pm_eval(p, av[i], ctx, allow_bare, &avals[i], &auc, depth + 1)) return 0;
        if (!sp_pre_eval_str_affix(&rv, strcmp(op, "start_with?") == 0, avals, (int)ac, out)) return 0;
        *used_const |= auc;
        return 1;
      }
      if (ac == 1) {
        int rrc = 0; SpPreVal av0;
        if (!sp_pre_pm_eval(p, av[0], ctx, allow_bare, &av0, &rrc, depth + 1)) return 0;
        if (!sp_pre_eval_binary(&rv, op, &av0, out)) return 0;
        *used_const |= (rc | rrc);
        return 1;
      }
      return 0;
    }
    default: return 0;
  }
}

/* 1 if provably truthy, 0 if provably falsy, -1 otherwise.
 * When allow_pure_lit == 0, only predicates that depend on a provably fixed
 * compile-time constant (RUBY_ENGINE, RUBY_PLATFORM, RUBY_VERSION, -D NAME,
 * or target-fixed defined?) are folded. */
static int sp_pre_pm_truth(const pm_parser_t *p, const pm_node_t *node, const SpPreFileCtx *ctx,
                           int allow_bare, int allow_pure_lit) {
  int used = 0; SpPreVal v;
  if (!sp_pre_pm_eval(p, node, ctx, allow_bare, &v, &used, 0)) return -1;
  if (!allow_pure_lit && !used) return -1;
  return sp_pre_val_truthy(&v) ? 1 : 0;
}

typedef struct {
  const pm_parser_t *p;
  SpPreFileCtx *ctx;
  unsigned char *def_guarded;
  unsigned char *all_shad;
  int in_cli_def_dead;
  int in_pattern;
  int in_bo;
  int nest_depth;
} SpPreScan;

static void sp_pre_scan_note_const(SpPreScan *s, const char *nm, size_t len, int is_class_or_mod) {
  if (!nm || !len) return;
  if (len == 11 && memcmp(nm, "RUBY_ENGINE", 11) == 0) s->ctx->allow_engine = 0;
  else if (len == 13 && memcmp(nm, "RUBY_PLATFORM", 13) == 0) s->ctx->allow_platform = 0;
  else if (len == 12 && memcmp(nm, "RUBY_VERSION", 12) == 0) s->ctx->allow_version = 0;
  else if (!is_class_or_mod && len == 6 && memcmp(nm, "Object", 6) == 0) s->ctx->allow_object = 0;
  int di = sp_pre_define_index(nm, len);
  if (di >= 0 && di < s->ctx->ndefs) {
    if (s->in_cli_def_dead) s->def_guarded[di] = 1;
    else s->ctx->def_shadowed[di] = 1;
  }
}

/* Effective method name of a call, resolving send/__send__/public_send when its
   first argument is a Symbol or String literal (`*via` = 1 if dispatched via send). */
static int sp_pre_pm_sent_name(const pm_parser_t *p, const pm_call_node_t *c,
                               const char **out, size_t *len, int *via) {
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&p->constant_pool, c->name);
  *via = 0;
  if ((nm->length == 4 && memcmp(nm->start, "send", 4) == 0) ||
      (nm->length == 8 && memcmp(nm->start, "__send__", 8) == 0) ||
      (nm->length == 11 && memcmp(nm->start, "public_send", 11) == 0)) {
    const pm_node_t *a0 = c->arguments && c->arguments->arguments.size >= 1 ? c->arguments->arguments.nodes[0] : NULL;
    const pm_string_t *u = a0 && PM_NODE_TYPE(a0) == PM_SYMBOL_NODE ? &((const pm_symbol_node_t *)a0)->unescaped
                         : a0 && PM_NODE_TYPE(a0) == PM_STRING_NODE ? &((const pm_string_node_t *)a0)->unescaped : NULL;
    if (!u) return 0;
    *out = (const char *)pm_string_source(u);
    *len = pm_string_length(u);
    *via = 1;
    return 1;
  }
  *out = (const char *)nm->start;
  *len = nm->length;
  return 1;
}

static bool sp_pre_scan_visit(const pm_node_t *node, void *data) {
  SpPreScan *s = data;
  pm_node_type_t t = PM_NODE_TYPE(node);
  if (t == PM_IN_NODE) {
    const pm_in_node_t *n = (const pm_in_node_t *)node;
    int prev = s->in_pattern;
    s->in_pattern = 1;
    if (n->pattern) pm_visit_node(n->pattern, sp_pre_scan_visit, s);
    s->in_pattern = prev;
    if (n->statements) pm_visit_node((const pm_node_t *)n->statements, sp_pre_scan_visit, s);
    return false;
  }
  if (!s->in_pattern && s->ctx->ndefs > 0 && s->all_shad && (t == PM_IF_NODE || t == PM_UNLESS_NODE)) {
    int is_unless = t == PM_UNLESS_NODE;
    const pm_node_t *pred = is_unless ? ((const pm_unless_node_t *)node)->predicate : ((const pm_if_node_t *)node)->predicate;
    const pm_node_t *body = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->statements
                                      : (const pm_node_t *)((const pm_if_node_t *)node)->statements;
    const pm_node_t *other = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->else_clause
                                       : ((const pm_if_node_t *)node)->subsequent;
    /* Evaluate ONLY `defined?(NAME)` / `Object.const_defined?(:NAME)` for CLI -D
       defines (all -D values shadowed) so `NAME = "default" unless defined?(NAME)`
       marks its dead default write as guarded rather than shadowing NAME. */
    SpPreFileCtx dctx = { 1, 0, 0, 0, s->ctx->allow_object, 0, 0, 1, s->all_shad, s->ctx->ndefs };
    int tr = sp_pre_pm_truth(s->p, pred, &dctx, !s->in_bo, 0);
    if (tr >= 0) {
      int take_body = is_unless ? !tr : tr;
      pm_visit_node(pred, sp_pre_scan_visit, s);
      s->in_cli_def_dead += take_body ? 0 : 1;
      if (body) pm_visit_node(body, sp_pre_scan_visit, s);
      s->in_cli_def_dead -= take_body ? 0 : 1;
      s->in_cli_def_dead += take_body ? 1 : 0;
      if (other) pm_visit_node(other, sp_pre_scan_visit, s);
      s->in_cli_def_dead -= take_body ? 1 : 0;
      return false;
    }
  }
  switch (t) {
    case PM_CONSTANT_WRITE_NODE:
    case PM_CONSTANT_OPERATOR_WRITE_NODE:
    case PM_CONSTANT_OR_WRITE_NODE:
    case PM_CONSTANT_AND_WRITE_NODE:
    case PM_CONSTANT_TARGET_NODE: {
      pm_constant_id_t cid = t == PM_CONSTANT_WRITE_NODE ? ((const pm_constant_write_node_t *)node)->name
                           : t == PM_CONSTANT_OPERATOR_WRITE_NODE ? ((const pm_constant_operator_write_node_t *)node)->name
                           : t == PM_CONSTANT_OR_WRITE_NODE ? ((const pm_constant_or_write_node_t *)node)->name
                           : t == PM_CONSTANT_AND_WRITE_NODE ? ((const pm_constant_and_write_node_t *)node)->name
                           : ((const pm_constant_target_node_t *)node)->name;
      const pm_constant_t *nm = pm_constant_pool_id_to_constant(&s->p->constant_pool, cid);
      sp_pre_scan_note_const(s, (const char *)nm->start, nm->length, 0);
      break;
    }
    case PM_CONSTANT_PATH_WRITE_NODE:
    case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE:
    case PM_CONSTANT_PATH_OR_WRITE_NODE:
    case PM_CONSTANT_PATH_AND_WRITE_NODE:
    case PM_CONSTANT_PATH_TARGET_NODE: {
      const pm_node_t *tgt = t == PM_CONSTANT_PATH_WRITE_NODE ? (const pm_node_t *)((const pm_constant_path_write_node_t *)node)->target
                           : t == PM_CONSTANT_PATH_OPERATOR_WRITE_NODE ? (const pm_node_t *)((const pm_constant_path_operator_write_node_t *)node)->target
                           : t == PM_CONSTANT_PATH_OR_WRITE_NODE ? (const pm_node_t *)((const pm_constant_path_or_write_node_t *)node)->target
                           : t == PM_CONSTANT_PATH_AND_WRITE_NODE ? (const pm_node_t *)((const pm_constant_path_and_write_node_t *)node)->target
                           : node;
      if (t == PM_CONSTANT_PATH_TARGET_NODE) {
        const pm_constant_path_target_node_t *cpt = (const pm_constant_path_target_node_t *)node;
        const pm_constant_t *cn = pm_constant_pool_id_to_constant(&s->p->constant_pool, cpt->name);
        const char *pn = NULL; size_t pl = 0;
        int is_top = !cpt->parent || (sp_pre_pm_const_name(s->p, cpt->parent, &pn, &pl, 1) && pl == 6 && memcmp(pn, "Object", 6) == 0);
        sp_pre_scan_note_const(s, (const char *)cn->start, cn->length, !is_top);
      }
      else if (tgt && PM_NODE_TYPE(tgt) == PM_CONSTANT_PATH_NODE) {
        const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)tgt;
        const pm_constant_t *cn = pm_constant_pool_id_to_constant(&s->p->constant_pool, cp->name);
        const char *tn = NULL; size_t tl = 0;
        int is_top = sp_pre_pm_const_name(s->p, tgt, &tn, &tl, 1);
        sp_pre_scan_note_const(s, (const char *)cn->start, cn->length, !is_top);
      }
      break;
    }
    case PM_CLASS_NODE: case PM_MODULE_NODE: {
      int is_cls = t == PM_CLASS_NODE;
      const pm_node_t *cp = is_cls ? ((const pm_class_node_t *)node)->constant_path
                                   : ((const pm_module_node_t *)node)->constant_path;
      const pm_node_t *sup = is_cls ? ((const pm_class_node_t *)node)->superclass : NULL;
      const pm_node_t *body = is_cls ? ((const pm_class_node_t *)node)->body
                                     : ((const pm_module_node_t *)node)->body;
      if (cp && PM_NODE_TYPE(cp) == PM_CONSTANT_READ_NODE) {
        const pm_constant_t *cn = pm_constant_pool_id_to_constant(&s->p->constant_pool, ((const pm_constant_read_node_t *)cp)->name);
        sp_pre_scan_note_const(s, (const char *)cn->start, cn->length, 1);
      }
      else if (cp && PM_NODE_TYPE(cp) == PM_CONSTANT_PATH_NODE) {
        const pm_constant_t *cn = pm_constant_pool_id_to_constant(&s->p->constant_pool, ((const pm_constant_path_node_t *)cp)->name);
        sp_pre_scan_note_const(s, (const char *)cn->start, cn->length, 1);
      }
      const char *top_nm = NULL; size_t top_len = 0;
      int opens_top = sp_pre_pm_const_name(s->p, cp, &top_nm, &top_len, s->nest_depth == 0);
      if (!s->ctx->core_ops_reopened && body && opens_top && sp_pre_is_core_op_owner(top_nm, top_len)) {
        SpPreOpScan os = { s->p, 0 };
        pm_visit_node(body, sp_pre_pm_op_visit, &os);
        if (os.hit) s->ctx->core_ops_reopened = 1;
      }
      if (cp) pm_visit_node(cp, sp_pre_scan_visit, s);
      if (sup) pm_visit_node(sup, sp_pre_scan_visit, s);
      int is_bo = is_cls && sp_pre_pm_is_basic_object(s->p, sup);
      int is_top_obj = is_cls && opens_top && top_len == 6 && memcmp(top_nm, "Object", 6) == 0;
      s->in_bo += is_bo;
      if (!is_top_obj) s->nest_depth++;
      if (body) pm_visit_node(body, sp_pre_scan_visit, s);
      if (!is_top_obj) s->nest_depth--;
      s->in_bo -= is_bo;
      return false;
    }
    case PM_SINGLETON_CLASS_NODE: {
      const pm_singleton_class_node_t *sc = (const pm_singleton_class_node_t *)node;
      if (sc->expression) pm_visit_node(sc->expression, sp_pre_scan_visit, s);
      s->nest_depth++;
      if (sc->body) pm_visit_node(sc->body, sp_pre_scan_visit, s);
      s->nest_depth--;
      return false;
    }
    case PM_CALL_NODE: {
      const pm_call_node_t *c = (const pm_call_node_t *)node;
      const char *mn = NULL; size_t ml = 0; int via = 0;
      if (!sp_pre_pm_sent_name(s->p, c, &mn, &ml, &via)) break;
      if (ml == 12 && memcmp(mn, "remove_const", 12) == 0) {
        s->ctx->has_remove_const = 1;
      }
      else if (ml == 9 && memcmp(mn, "const_set", 9) == 0) {
        size_t ai = (size_t)via;
        const pm_node_t *a0 = c->arguments && c->arguments->arguments.size > ai ? c->arguments->arguments.nodes[ai] : NULL;
        const pm_string_t *u = a0 && PM_NODE_TYPE(a0) == PM_SYMBOL_NODE ? &((const pm_symbol_node_t *)a0)->unescaped
                             : a0 && PM_NODE_TYPE(a0) == PM_STRING_NODE ? &((const pm_string_node_t *)a0)->unescaped : NULL;
        if (u) sp_pre_scan_note_const(s, (const char *)pm_string_source(u), pm_string_length(u), 0);
        else if (s->ctx->ndefs > 0) memset(s->ctx->def_shadowed, 1, (size_t)s->ctx->ndefs);
      }
      else if (!s->ctx->core_ops_reopened && c->receiver &&
               ((ml == 13 && memcmp(mn, "define_method", 13) == 0) ||
                (ml == 12 && memcmp(mn, "alias_method", 12) == 0) ||
                (ml == 12 && memcmp(mn, "undef_method", 12) == 0) ||
                (ml == 13 && memcmp(mn, "remove_method", 13) == 0) ||
                (ml == 7 && memcmp(mn, "include", 7) == 0) ||
                (ml == 7 && memcmp(mn, "prepend", 7) == 0) ||
                (ml == 10 && memcmp(mn, "class_eval", 10) == 0) ||
                (ml == 11 && memcmp(mn, "module_eval", 11) == 0)) &&
               sp_pre_pm_is_core_op_owner(s->p, c->receiver)) {
        s->ctx->core_ops_reopened = 1;
      }
      break;
    }
    default: break;
  }
  return true;
}

static void sp_pre_scan_file(const pm_parser_t *p, const pm_node_t *root,
                             const SpPreFileCtx *inherit, SpPreFileCtx *out) {
  memset(out, 0, sizeof *out);
  int nd = sp_pre_define_count();
  out->initialized = 1;
  out->allow_engine = inherit && inherit->initialized ? inherit->allow_engine : 1;
  out->allow_platform = inherit && inherit->initialized ? inherit->allow_platform : 1;
  out->allow_version = inherit && inherit->initialized ? inherit->allow_version : 1;
  out->allow_object = inherit && inherit->initialized ? inherit->allow_object : 1;
  out->has_remove_const = inherit && inherit->initialized ? inherit->has_remove_const : 0;
  out->core_ops_reopened = inherit && inherit->initialized ? inherit->core_ops_reopened : 0;
  out->ndefs = nd;
  if (nd > 0) {
    out->def_shadowed = calloc((size_t)nd, 1);
    if (!out->def_shadowed) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    if (inherit && inherit->def_shadowed)
      memcpy(out->def_shadowed, inherit->def_shadowed, (size_t)(nd < inherit->ndefs ? nd : inherit->ndefs));
  }
  if (!root) return;
  unsigned char *guarded = nd > 0 ? calloc((size_t)nd, 1) : NULL;
  unsigned char *all_shad = nd > 0 ? malloc((size_t)nd) : NULL;
  if (nd > 0 && (!guarded || !all_shad)) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (all_shad) memset(all_shad, 1, (size_t)nd);
  SpPreScan s = { p, out, guarded, all_shad, 0, 0, 0, 0 };
  pm_visit_node(root, sp_pre_scan_visit, &s);
  if (out->has_remove_const || out->core_ops_reopened || !out->allow_object) {
    for (int i = 0; i < nd; i++) if (guarded[i]) out->def_shadowed[i] = 1;
  }
  free(guarded);
  free(all_shad);
}

/* Collect depth-0 local variable names bound inside a dead Prism subtree so
 * Ruby's lexical local-variable declaration rules hold (`if false; x = 1; end;
 * defined?(x) == "local-variable" && x.nil?`). */
typedef struct {
  const pm_parser_t *p;
  char *buf;
  size_t len, cap;
} SpDeadLocalWalk;

static void sp_dead_local_add(SpDeadLocalWalk *w, pm_constant_id_t cid) {
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&w->p->constant_pool, cid);
  if (!nm->length) return;
  size_t need = w->len + (w->len ? 1 : 0) + nm->length + 1;
  if (need > w->cap) {
    size_t ncap = w->cap ? w->cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    char *nb = realloc(w->buf, ncap);
    if (!nb) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    w->buf = nb; w->cap = ncap;
  }
  if (w->len) w->buf[w->len++] = ',';
  memcpy(w->buf + w->len, nm->start, nm->length);
  w->len += nm->length;
  w->buf[w->len] = '\0';
}

static bool sp_dead_local_visit(const pm_node_t *node, void *data) {
  SpDeadLocalWalk *w = data;
  switch (PM_NODE_TYPE(node)) {
    case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE:
    case PM_SINGLETON_CLASS_NODE: case PM_BLOCK_NODE: case PM_LAMBDA_NODE:
      return false;
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
      const pm_local_variable_write_node_t *n = (const pm_local_variable_write_node_t *)node;
      if (n->depth == 0) sp_dead_local_add(w, n->name);
      return true;
    }
    case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
      const pm_local_variable_or_write_node_t *n = (const pm_local_variable_or_write_node_t *)node;
      if (n->depth == 0) sp_dead_local_add(w, n->name);
      return true;
    }
    case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
      const pm_local_variable_and_write_node_t *n = (const pm_local_variable_and_write_node_t *)node;
      if (n->depth == 0) sp_dead_local_add(w, n->name);
      return true;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
      const pm_local_variable_operator_write_node_t *n = (const pm_local_variable_operator_write_node_t *)node;
      if (n->depth == 0) sp_dead_local_add(w, n->name);
      return true;
    }
    case PM_LOCAL_VARIABLE_TARGET_NODE: {
      const pm_local_variable_target_node_t *n = (const pm_local_variable_target_node_t *)node;
      if (n->depth == 0) sp_dead_local_add(w, n->name);
      return false;
    }
    default: return true;
  }
}

static void sp_pre_pm_dead_locals(const pm_parser_t *p, const pm_node_t *dead_node, int cond_id) {
  if (!dead_node) return;
  SpDeadLocalWalk w = { p, NULL, 0, 0 };
  pm_visit_node(dead_node, sp_dead_local_visit, &w);
  if (w.buf && w.len) {
    /* the names are as written, in the scope the branch is in: its decl, for comp_name_locals */
    int d = nt_build_scope_at(out_nt, 0);
    if (d >= 0) emit_int(cond_id, "decl", d);
    const char *prev = nt_str(out_nt, cond_id, "dead_locals");
    if (prev && *prev) {
      size_t pl = strlen(prev);
      char *merged = malloc(pl + 1 + w.len + 1);
      if (merged) {
        memcpy(merged, prev, pl);
        merged[pl] = ',';
        memcpy(merged + pl + 1, w.buf, w.len + 1);
        emit_str(cond_id, "dead_locals", merged);
        free(merged);
      }
    }
    else emit_str(cond_id, "dead_locals", w.buf);
  }
  free(w.buf);
}

static void sp_pre_emit_cli_defines(int root_id, int after_boot) {
  int nd = sp_pre_define_count();
  if (nd <= 0 || root_id < 0) return;
  int st = nt_ref(out_nt, root_id, "statements");
  if (st < 0) {
    st = node_counter++;
    nt_put_type(out_nt, st, "StatementsNode");
    nt_put_ref(out_nt, root_id, "statements", st);
  }
  int *cids = malloc(sizeof(int) * (size_t)nd);
  if (!cids) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int i = 0; i < nd; i++) {
    const char *nm = sp_pre_define_name(i);
    const SpPreVal *v = sp_pre_define_val(i);
    int vid = node_counter++;
    if (v->kind == SP_PRE_BOOL) {
      nt_put_type(out_nt, vid, v->b ? "TrueNode" : "FalseNode");
    }
    else if (v->kind == SP_PRE_NIL) {
      nt_put_type(out_nt, vid, "NilNode");
    }
    else if (v->kind == SP_PRE_INT) {
      nt_put_type(out_nt, vid, "IntegerNode");
      emit_int(vid, "value", v->i);
      if (sp_target_int_bits == 32 && (v->i < INT32_MIN || v->i > INT32_MAX)) {
        char bbuf[32];
        snprintf(bbuf, sizeof bbuf, "%lld", v->i);
        emit_str(vid, "bigval", bbuf);
      }
    }
    else {
      nt_put_type(out_nt, vid, "StringNode");
      emit_str(vid, "content", v->s ? v->s : "");
      emit_int(vid, "fzl", 1);
    }
    int wid = node_counter++;
    nt_put_type(out_nt, wid, "ConstantWriteNode");
    emit_str(wid, "name", nm);
    nt_put_ref(out_nt, wid, "value", vid);
    emit_int(wid, "cli_define", 1);
    cids[i] = wid;
  }
  int n = 0;
  const int *v = nt_arr(out_nt, st, "body", &n);
  int at = after_boot < 0 ? 0 : (after_boot > n ? n : after_boot);
  int *nv = malloc(sizeof(int) * (size_t)(n + nd));
  if (!nv) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (at > 0) memcpy(nv, v, sizeof(int) * (size_t)at);
  memcpy(nv + at, cids, sizeof(int) * (size_t)nd);
  if (n > at) memcpy(nv + at + nd, v + at, sizeof(int) * (size_t)(n - at));
  nt_node_set_arr(out_nt, st, "body", nv, n + nd);
  free(nv);
  free(cids);
}

static void flat_cond(int id, pm_node_t *node, int is_unless) {
  pm_node_t *pred = is_unless ? ((pm_unless_node_t *)node)->predicate : ((pm_if_node_t *)node)->predicate;
  pm_node_t *body = is_unless ? (pm_node_t *)((pm_unless_node_t *)node)->statements
                              : (pm_node_t *)((pm_if_node_t *)node)->statements;
  pm_node_t *other = is_unless ? (pm_node_t *)((pm_unless_node_t *)node)->else_clause
                               : ((pm_if_node_t *)node)->subsequent;
  const char *other_k = is_unless ? "else_clause" : "subsequent";
  nt_put_type(out_nt, id, is_unless ? "UnlessNode" : "IfNode");
  if (!g_in_pattern && g_flat_pre) {
    int truth = sp_pre_pm_truth(g_parser, pred, g_flat_pre, !g_in_basic_object, 0);
    if (truth >= 0) {
      int pid = node_counter++;
      nt_put_type(out_nt, pid, truth ? "TrueNode" : "FalseNode");
      emit_int(pid, "engine_check", 1);
      if (g_emit_line)
        emit_int(pid, "node_line", (int)pm_newline_list_line(&g_parser->newline_list, pred->location.start, g_parser->start_line));
      nt_put_ref(out_nt, id, "predicate", pid);
      sp_pre_pm_dead_locals(g_parser, pred, id);
      int take_body = is_unless ? !truth : truth;
      if (take_body) {
        emit_ref(id, "statements", body);
        nt_put_ref(out_nt, id, other_k, -1);
        if (other) sp_pre_pm_dead_locals(g_parser, other, id);
      }
      else {
        nt_put_ref(out_nt, id, "statements", -1);
        emit_ref(id, other_k, other);
        if (body) sp_pre_pm_dead_locals(g_parser, body, id);
      }
      return;
    }
  }
  emit_ref(id, "predicate", pred);
  emit_ref(id, "statements", body);
  emit_ref(id, other_k, other);
}

static void flat_in(int id, pm_in_node_t *n) {
  nt_put_type(out_nt, id, "InNode");
  n->pattern = flat_pattern(n->pattern);
  int prev = g_in_pattern;
  g_in_pattern = 1;
  emit_ref(id, "pattern", n->pattern);
  g_in_pattern = prev;
  emit_ref(id, "statements", (pm_node_t *)n->statements);
}

static void flat_class(int id, pm_class_node_t *n) {
  nt_put_type(out_nt, id, "ClassNode");
  out_scope_locals(id, &n->locals);
  emit_ref(id, "constant_path", n->constant_path);
  emit_ref(id, "superclass", flat_superclass(n->superclass));
  int is_bo = sp_pre_pm_is_basic_object(g_parser, n->superclass);
  g_in_basic_object += is_bo;
  flat_scoped(id, "body", n->body);
  g_in_basic_object -= is_bo;
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
    int fid = g_flat_file;
    if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n && sp_line_orig[bl] > 0) {
      orig = sp_line_orig[bl];
      fid = sp_line_file[bl];
      if (sp_disp_line[bl] > 0) {
        /* The file the node is written in, kept apart from the marker's:
           the load graph places a node by the file that holds it */
        if (sp_disp_file[bl] != fid) emit_int(id, "node_src_file", (long long)fid);
        orig = sp_disp_line[bl]; fid = sp_disp_file[bl];
      }
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
    flat_scoped(id, "statements", (pm_node_t *)n->statements);
    break;
  }
  case PM_STATEMENTS_NODE:
    N("StatementsNode");
    flatten_statements(id, &((pm_statements_node_t *)node)->body);
    break;
  case PM_CLASS_NODE:
    flat_class(id, (pm_class_node_t *)node);
    break;
  case PM_MODULE_NODE: {
    pm_module_node_t *n = (pm_module_node_t *)node;
    N("ModuleNode");
    out_scope_locals(id, &n->locals);
    R("constant_path", n->constant_path);
    flat_scoped(id, "body", n->body);
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
    flat_scoped(id, "body", n->body);
    break;
  }
  case PM_DEF_NODE:
    N("DefNode");
    flat_def(id, (const pm_def_node_t *)node);
    break;
  case PM_CALL_NODE: {
    pm_call_node_t *n = (pm_call_node_t *)node;
    if (sp_flatten_call_as(id, n)) break;
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
    emit_local(id, n->name, n->depth); I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_READ_NODE: {
    pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
    N("LocalVariableReadNode");
    emit_local(id, n->name, n->depth); I("depth", n->depth);
    break;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
    N("LocalVariableOperatorWriteNode");
    emit_local(id, n->name, n->depth); I("depth", n->depth);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
    N("LocalVariableOrWriteNode");
    emit_local(id, n->name, n->depth); I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
    N("LocalVariableAndWriteNode");
    emit_local(id, n->name, n->depth); I("depth", n->depth);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)node;
    N("LocalVariableTargetNode");
    emit_local(id, n->name, n->depth); I("depth", n->depth);
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
    emit_attr_names(id, n->read_name, n->write_name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    emit_call_flags(id, node);
    break;
  }
  case PM_CALL_AND_WRITE_NODE: {
    pm_call_and_write_node_t *n = (pm_call_and_write_node_t *)node;
    N("CallAndWriteNode");
    R("receiver", n->receiver);
    emit_attr_names(id, n->read_name, n->write_name);
    R("value", n->value);
    emit_call_flags(id, node);
    break;
  }
  case PM_CALL_OR_WRITE_NODE: {
    pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)node;
    N("CallOrWriteNode");
    R("receiver", n->receiver);
    emit_attr_names(id, n->read_name, n->write_name);
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
  case PM_IF_NODE:
    flat_cond(id, node, 0);
    break;
  case PM_ELSE_NODE: {
    pm_else_node_t *n = (pm_else_node_t *)node;
    N("ElseNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNLESS_NODE:
    flat_cond(id, node, 1);
    break;
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
  case PM_IN_NODE:
    flat_in(id, (pm_in_node_t *)node);
    break;
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
    N("BlockNode"); decl_push(id);
    if (!n->opening_loc.start) I("sym_proc_block", 1);  /* an &:name block (sp_sym_proc_block) has no `{` */
    /* Block-local variables (params + first-assigned-inside): Ruby scoping
       makes non-param locals FRESH on every block invocation; codegen needs
       the list to reset them per iteration in fused loops. */
    out_scope_locals(id, &n->locals);
    /* Serialize block parameters */
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        int bpid = node_counter++;
        nt_put_type(out_nt, bpid, "BlockParametersNode");
        if (in_bi) emit_int(bpid, "node_bi", g_bi_base + 1);
        if (bp->parameters) emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        emit_node_array(bpid, "locals", &bp->locals);   /* (the `; x` declarations, apart from the scope's names above) */
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
    R("body", n->body); decl_pop();
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
    decl_push(id); out_scope_locals(id, &n->locals);
    /* a lambda holds no numbered-parameters node: how many `_1`.. it reads is on the lambda */
    if (n->parameters && PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE)
      I("numbered_max", ((pm_numbered_parameters_node_t *)n->parameters)->maximum);
    else if (n->parameters && PM_NODE_TYPE(n->parameters) == PM_IT_PARAMETERS_NODE) { I("numbered_max", 1); I("implicit_it", 1); }
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        if (bp->parameters) R("parameters", bp->parameters);
        A("block_locals", &bp->locals);   /* (a lambda holds no BlockParametersNode to carry them) */
      }
else if (PM_NODE_TYPE(n->parameters) != PM_NUMBERED_PARAMETERS_NODE && PM_NODE_TYPE(n->parameters) != PM_IT_PARAMETERS_NODE) {
        R("parameters", n->parameters);
      }
    }
    if (n->body) R("body", n->body);
    decl_pop();
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
    n->pattern = flat_pattern(n->pattern);
    if (n->pattern && PM_NODE_TYPE_P(n->pattern, PM_LOCAL_VARIABLE_TARGET_NODE)) {
      pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->pattern;
      N("LocalVariableWriteNode");
      emit_local(id, t->name, t->depth);
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
    n->pattern = flat_pattern(n->pattern);
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
    emit_it_parameters(id);
    break;
  case PM_IT_LOCAL_VARIABLE_READ_NODE:
    /* `it` inside the block body. Lowered to a regular
       LocalVariableReadNode named "_1" so it pairs with the
       lowered NumberedParametersNode { maximum: 1 } above; implicit_it keeps it
       apart from a written `_1` (blockarg_plain_read). */
    N("LocalVariableReadNode"); I("implicit_it", 1);
    emit_local_s(id, "_1", 0);
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
    /* `END { ... }`. CRuby puts the block on the same stack as at_exit's
       hooks, when the statement runs and only the first time, and runs that
       stack last in first out at exit. So it is read as `at_exit { ... }`,
       marked end_once for codegen to register it on the first run only. */
    pm_post_execution_node_t *n = (pm_post_execution_node_t *)node;
    N("CallNode");
    emit_str(id, "name", "at_exit");
    I("end_once", 1);
    int bid = node_counter++;
    nt_put_type(out_nt, bid, "BlockNode");
    if (in_bi) emit_int(bid, "node_bi", g_bi_base + 1);
    emit_ref(bid, "body", (pm_node_t *)n->statements);
    nt_put_ref(out_nt, id, "block", bid);
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

/* A call flattened as node `id` of another kind: `__FILE__ == $0` and its
   spellings as the constant they answer, stamped program_guard; and a
   require as its LoadNode, which carries its path when
   it is one literal (or the one path its expression names), else the
   expression that computes it when it runs, and what its site resolved to
   before the file was flattened (sp_graph_site_take). 0 for a call
   flattened as a CallNode. */
static int sp_flatten_call_as(int id, pm_call_node_t *n) {
  pm_node_t *node = (pm_node_t *)n;
  int pg = sp_program_guard(node);
  if (pg) {
    nt_put_type(out_nt, id, pg == 1 ? "TrueNode" : "FalseNode");
    emit_int(id, "program_guard", 1);
    return 1;
  }
  if (!sp_load_call(n)) return 0;
  if (sp_pm_name_is(g_parser, n->name, "autoload")) {
    sp_graph_site_take(id, node);
    return 0;   /* the registration belongs to the ordinary CallNode */
  }
  pm_node_t *arg = n->arguments->arguments.nodes[0];
  nt_put_type(out_nt, id, "LoadNode");
  char *m = cstr(n->name);
  emit_str(id, "method", m);
  free(m);
  emit_int(id, "line", pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line));
  if (PM_NODE_TYPE(arg) == PM_STRING_NODE) emit_pm_string(id, "path", &((pm_string_node_t *)arg)->unescaped);
  /* (for a class's own require, desugar_owned_require_loads: Kernel.require
     stays Kernel's, and a literal path keeps its frozen state) */
  if (n->receiver) emit_int(id, "kernel_recv", 1);
  if (PM_NODE_TYPE(arg) == PM_STRING_NODE && sp_node_fsl(arg)) emit_int(id, "path_fzl", 1);
  if (sp_graph_site_take(id, node)) emit_ref(id, "argument", arg);
  return 1;
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

/* ---- strict-require: features enabled by a `require "name"` ----
   SPINEL_STRICT_REQUIRE: when set, a require-only stdlib feature (stringio,
   io/console, ...) is provided only if its `require` textually appears in the
   program, matching CRuby's uninitialized-constant / NoMethodError. Default off
   keeps the historical always-available behaviour. The feature name is the
   require argument verbatim ("stringio", "io/console"). Marked as each plain
   require is resolved (file-backed or native); read at analyze/codegen time via
   sp_feature_enabled(). Definitions here (spinel_parse.c includes no project
   headers); declared extern in compiler.h. */
int g_strict_require = 0;
int g_strict_require_cli = 0;   /* --strict-require */
int g_dump_load_graph = 0;    /* --dump-load-graph */
int sp_parse_reported = 0;    /* the parse failed and said why (the driver adds nothing) */
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

/* The required features by name: an open-addressed set of indices into
   sp_req_feats, kept at most half full. The analysis asks it for each of
   the method tables' libraries at every builtin-table lookup (bmt_loaded),
   and a scan of the list made that grow with the calls times the files a
   program requires (#11722). */
static int *sp_req_slots = NULL;
static int sp_req_nslots = 0;
static unsigned sp_req_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}
static int sp_req_find(const char *name) {
  if (!sp_req_nslots) return -1;
  for (unsigned i = sp_req_hash(name) & (unsigned)(sp_req_nslots - 1);; i = (i + 1) & (unsigned)(sp_req_nslots - 1)) {
    int k = sp_req_slots[i];
    if (k < 0) return -1;
    if (strcmp(sp_req_feats[k], name) == 0) return k;
  }
}
static void sp_req_place(int k) {
  unsigned i = sp_req_hash(sp_req_feats[k]) & (unsigned)(sp_req_nslots - 1);
  while (sp_req_slots[i] >= 0) i = (i + 1) & (unsigned)(sp_req_nslots - 1);
  sp_req_slots[i] = k;
}

void sp_feature_mark(const char *name) {
  if (!name || sp_req_find(name) >= 0) return;
  sp_req_feats = sp_grow_strs(sp_req_feats, sp_req_feats_n, &sp_req_feats_cap, 64);
  sp_req_feats[sp_req_feats_n++] = strdup(name);
  if (2 * sp_req_feats_n > sp_req_nslots) {
    free(sp_req_slots);
    sp_req_nslots = sp_req_nslots ? sp_req_nslots * 2 : 128;
    sp_req_slots = (int *)malloc(sizeof(int) * (size_t)sp_req_nslots);
    if (!sp_req_slots) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    for (int i = 0; i < sp_req_nslots; i++) sp_req_slots[i] = -1;
    for (int k = 0; k < sp_req_feats_n; k++) sp_req_place(k);
  }
  else sp_req_place(sp_req_feats_n - 1);
}

int sp_feature_enabled(const char *name) {
  if (!g_strict_require) return 1;   /* gate off: always provide (baseline) */
  if (!name) return 1;
  return sp_req_find(name) >= 0;
}

/* Like sp_feature_enabled, but the require is mandatory REGARDLESS of the
   strict-require mode: true only when the program actually `require`d the
   feature. For stdlib whose classes CRuby only defines after the require
   (socket's TCPServer/TCPSocket) -- without this, the baseline mode would
   grow the constants unconditionally and diverge from CRuby's NameError. */
int sp_feature_required(const char *name) {
  return name && sp_req_find(name) >= 0;
}
/* How many features the program has required: a set that only grows, so
   an answer made from them holds while the count stays (bmt_loaded). */
int sp_features_required_count(void) {
  return sp_req_feats_n;
}

/* Feature search roots from `-I <dir>` (like ruby -I). A plain `require "X"`
   that is not a bundled lib or native feature is looked up here, in order, as
   <root>/X.rb (CRuby single-file form) or <root>/X/<last>.rb (the colocated
   directory form, where one feature's sources share a directory). */
static char **sp_feature_roots = NULL;
static int sp_feature_roots_n = 0;
static int sp_feature_roots_cap = 0;

static void sp_feature_root_at(const char *dir, int first) {
  if (!dir) return;
  sp_feature_roots = sp_grow_strs(sp_feature_roots, sp_feature_roots_n, &sp_feature_roots_cap, 16);
  if (first) memmove(sp_feature_roots + 1, sp_feature_roots, sizeof *sp_feature_roots * (size_t)sp_feature_roots_n);
  sp_feature_roots[first ? 0 : sp_feature_roots_n] = strdup(dir);
  sp_feature_roots_n++;
}
void sp_add_feature_root(const char *dir) { sp_feature_root_at(dir, 0); }

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

/* A generated .rb names the source its lines came from (#7630):
   `#<SPINEL_SOURCE>greeting.html.erb:12` pins file and line for the lines
   after it, until the next marker or the end of the file it is in. */
#define SP_SOURCE_PREFIX "#<SPINEL_SOURCE>"

/* A builtins/ file is a root of its own: is the file being flattened one? */
static int sp_in_builtin(const uint8_t *at) {
  (void)at;
  return g_flat_builtin;
}

static char **sp_file_table = NULL;  /* id -> path (declared above flatten) */
static unsigned char *sp_file_tier = NULL;   /* id -> where `require` found it (SP_LIB_*) */
static int sp_file_count = 0, sp_file_cap = 0;

/* A file's tier once another require finds it too (one canonical path,
   reached through two spellings): the later of the two in SP_LIB_*'s
   order, so a builtin's or a package's stays that once found; SP_LIB_NONE
   (a require_relative, the entry) changes nothing. */
static int sp_tier_merge(int old, int tier) { return tier > old ? tier : old; }

static int sp_new_file(const char *path, int tier) {
  int cap = sp_file_cap;
  sp_file_table = sp_grow_strs(sp_file_table, sp_file_count, &sp_file_cap, 8);
  if (sp_file_cap != cap) {
    sp_file_tier = realloc(sp_file_tier, (size_t)sp_file_cap);
    sp_file_defer_force = realloc(sp_file_defer_force, (size_t)sp_file_cap);
  }
  char *dup_path = strdup(path);
  if (!dup_path || !sp_file_tier || !sp_file_defer_force) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_file_table[sp_file_count] = dup_path;
  sp_file_tier[sp_file_count] = (unsigned char)tier;
  sp_file_defer_force[sp_file_count] = 0;
  return sp_file_count++;
}

static int sp_intern_file(const char *path, int tier) {
  for (int i = 0; i < sp_file_count; i++)
    if (strcmp(sp_file_table[i], path) == 0) {
      sp_file_tier[i] = (unsigned char)sp_tier_merge(sp_file_tier[i], tier);
      return i;
    }
  return sp_new_file(path, tier);
}

/* The file id a `#<SPINEL_SOURCE>` marker in physical file `phys` shows: the
   one for the name it gives, as the table has it. The name says nothing about
   which file's `# spinel: defer-refusals` governs the lines under it, so a
   name the table already has for a file of the other answer (the marker's
   file defers and the name's file does not, or the reverse) gets an id of its
   own, the same path, that answers as `phys` does. Every copy of the position
   a node made then carries the answer with it.
   A path has at most two ids, and the first, made by sp_intern_file, is
   never one of these: a file read by that path is found by it and keeps its
   own answer and tier, so a marker's own id always comes after its path's
   first id. */
static int sp_intern_display_file(const char *path, int phys) {
  int want = phys >= 0 && phys < sp_file_count && sp_file_id_defers(phys);
  int first = sp_intern_file(path, SP_LIB_NONE);
  /* id 0 is the entry script, and also what the frame stack holds for "no
     marker": a marker naming it is not taken, and its lines keep the id of
     the file they are in, which answers as that file does */
  if (first == 0) return 0;
  for (int i = first; i < sp_file_count; i++) {
    if (strcmp(sp_file_table[i], path) != 0) continue;
    if (sp_file_id_defers(i) == want) return i;
  }
  int id = sp_new_file(path, SP_LIB_NONE);
  sp_file_defer_force[id] = want ? 2 : 1;
  return id;
}

/* The file table's tiers, onto the node table's */
static void sp_file_tiers_mark(NodeTable *nt) {
  for (int i = 0; i < sp_file_count; i++) {
    int src = i;   /* an id made for a marker's name has its path's first id's tier */
    if (sp_file_defer_force && sp_file_defer_force[i])
      for (int k = 0; k < i; k++) if (strcmp(sp_file_table[k], sp_file_table[i]) == 0) { src = k; break; }
    if (sp_file_tier[src] != SP_LIB_NONE) nt_put_file_tier(nt, i, sp_file_tier[src]);
  }
}
/* The line map of one file's text: each line's file and line, and the
   position a `#<SPINEL_SOURCE>file:line` marker pins for the lines after it.
   file is the text's own path. */
static void sp_build_line_map(const char *src, const char *file) {
  size_t nlines = 1;
  for (const char *p = src; *p; p++) if (*p == '\n') nlines++;
  sp_line_map_n = (int)nlines;
  sp_line_file = (int *)calloc(nlines + 2, sizeof(int));
  sp_line_orig = (int *)calloc(nlines + 2, sizeof(int));
  sp_disp_file = (int *)calloc(nlines + 2, sizeof(int));
  sp_disp_line = (int *)calloc(nlines + 2, sizeof(int));
  if (!sp_line_file || !sp_line_orig || !sp_disp_file || !sp_disp_line) {
    fprintf(stderr, "spinel_parse: out of memory\n"); exit(1);
  }
  int fid = sp_intern_file(file, SP_LIB_NONE), dfile = 0, dline = 0;   /* dfile: the pinned file id, 0 = none */
  int bl = 1;
  const char *line = src;
  while (1) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    sp_line_file[bl] = fid;
    sp_line_orig[bl] = bl;
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
        dfile = sp_intern_display_file(pathbuf, fid);
        dline = (int)ln;
      }
    }
    else if (dfile) {
      sp_disp_file[bl] = dfile;
      sp_disp_line[bl] = dline;
    }
    bl++;
    if (!eol) break;
    line = eol + 1;
    if (*line == '\0') break;
  }
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

/* Dependency resolution precedes analysis, so even `if false; require ...`
   would otherwise read the file (or reject a missing one). Only erase literal
   require calls in provably unreachable branches: keep the surrounding code
   for Ruby's lexical locals, and preserve every newline for the source map. */
/* The call nodes the fold blanked, so the require sites read from the same
   tree leave them out (sp_req_sites). */
typedef struct {
  const pm_node_t **v;
  int n, cap;
} SpNodeList;

typedef struct {
  pm_parser_t *parser;
  const char *source;
  char *result;
  int dead;
  int in_bo;
  const SpPreFileCtx *pre;
  SpNodeList *blanked;
} SpDeadRequire;

static int sp_pm_name_is(const pm_parser_t *parser, pm_constant_id_t id, const char *word) {
  const pm_constant_t *name = pm_constant_pool_id_to_constant(&parser->constant_pool, id);
  size_t n = strlen(word);
  return name->length == n && memcmp(name->start, word, n) == 0;
}

/* `Kernel.require "x"` (`::Kernel.` too) is the bare require: the textual
   resolver has treated it so since 0c0f61fff, so a dead branch must drop it
   exactly as it drops a receiver-less `require`, or --strict-require refuses
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
  SpNodeList *b = ctx->blanked;
  if (b->n == b->cap) {
    b->cap = b->cap ? b->cap * 2 : 8;
    b->v = realloc(b->v, sizeof *b->v * (size_t)b->cap);
    if (!b->v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  b->v[b->n++] = node;
  size_t start = (size_t)(node->location.start - (const uint8_t *)ctx->source);
  size_t end = (size_t)(node->location.end - (const uint8_t *)ctx->source);
  for (size_t i = start; i < end; i++)
    if (ctx->result[i] != '\n' && ctx->result[i] != '\r') ctx->result[i] = ' ';
  memcpy(ctx->result + start, "(nil", 4);
  ctx->result[end - 1] = ')';
}

static bool sp_skip_dead_require(const pm_node_t *node, void *data) {
  SpDeadRequire *ctx = data;
  if (PM_NODE_TYPE(node) == PM_CLASS_NODE) {
    const pm_class_node_t *n = (const pm_class_node_t *)node;
    if (n->constant_path) pm_visit_node(n->constant_path, sp_skip_dead_require, ctx);
    if (n->superclass) pm_visit_node(n->superclass, sp_skip_dead_require, ctx);
    int is_bo = sp_pre_pm_is_basic_object(ctx->parser, n->superclass);
    ctx->in_bo += is_bo;
    if (n->body) pm_visit_node(n->body, sp_skip_dead_require, ctx);
    ctx->in_bo -= is_bo;
    return false;
  }
  if (PM_NODE_TYPE(node) == PM_IN_NODE) {
    const pm_in_node_t *n = (const pm_in_node_t *)node;
    if (n->pattern) {
      SpDeadRequire pat = *ctx;
      if (!ctx->dead) pat.pre = NULL;
      pm_visit_node(n->pattern, sp_skip_dead_require, &pat);
    }
    if (n->statements) pm_visit_node((const pm_node_t *)n->statements, sp_skip_dead_require, ctx);
    return false;
  }
  if (!ctx->dead && ctx->pre && (PM_NODE_TYPE(node) == PM_IF_NODE || PM_NODE_TYPE(node) == PM_UNLESS_NODE)) {
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
    int truth = sp_pre_pm_truth(ctx->parser, predicate, ctx->pre, !ctx->in_bo, 1);
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
    if (!call->block && call->arguments && call->arguments->arguments.size == 2 &&
        name->length == 8 && memcmp(name->start, "autoload", 8) == 0) {
      const pm_node_t *arg = call->arguments->arguments.nodes[1];
      if (PM_NODE_TYPE(arg) == PM_STRING_NODE) {
        const pm_string_node_t *str = (const pm_string_node_t *)arg;
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

/* g_require_unread: may the program load a file this compiler did not read?
   Such a file can define or reopen anything, so a pass that asks what the
   program gives a class has to take the answer as unknown where this is set.
   It stays clear only where the program proves there is no such file, read as
   written (ahead of every rewrite below) in each file handed to
   resolve_requires, up to the file's `__END__` line:
     - none of $LOAD_PATH, $:, $-I, $LOADED_FEATURES, $" is read or written
       (a variable node of the file's tree), so a name is looked up where
       this compiler looks;
     - no call is named load or autoload (a call node of the tree);
     - every require and require_relative call of the tree was resolved: a
       file read, one read before, or a feature the runtime provides. One
       left as a raise or a warning, blanked as unreachable, or past where
       the scan gave up counts as written and not resolved, and so does a
       LoadNode that names no file; one a
       rewrite made sets the flag itself.
   The text the class macros write (sp_expand_class_macros) is parsed once
   they have, for the load and load-path nodes: the parts a macro joins need
   not spell one. The flag is final when the parser returns.
   All of it is read from the trees, so the word in a comment or a string
   sets nothing. Declared extern in compiler.h. */
int g_require_unread = 0;
/* The features this program was compiled with, as each require and
   require_relative names it: a require whose path is known only at run
   time answers for these as already loaded, and raises LoadError for any
   other (sp_require_computed). */
const char **g_required_features = NULL;
int g_nrequired_features = 0;
static const char **sp_boot_features = NULL;
static int sp_nboot_features = 0;
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
/* Does the program require a feature that is not a core library its start
   loads? A core library is read like the program, so its defs are in the
   node table; another feature can be native and define methods none sees. */
int sp_requires_non_boot_feature(void) {
  for (int i = 0; i < g_nrequired_features; i++) {
    /* (builtins/: the core methods written in Ruby, read like the program) */
    int boot = strncmp(g_required_features[i], "builtins/", 9) == 0;
    for (int k = 0; k < sp_nboot_features && !boot; k++) boot = strcmp(sp_boot_features[k], g_required_features[i]) == 0;
    if (!boot) return 1;
  }
  return 0;
}

/* The names of one file's tree that bear on g_require_unread: a call named
   load or autoload, a variable that is the load path or the loaded features
   (either sets the flag), and a call named require or require_relative
   (counted in sp_req_written when `count`). */
typedef struct { const pm_parser_t *parser; int count; const SpPreFileCtx *pre; int in_bo; } SpLoadSites;

static bool sp_load_sites_visit(const pm_node_t *node, void *data) {
  SpLoadSites *w = data;
  pm_constant_id_t id;
  int call = 0;
  switch (PM_NODE_TYPE(node)) {
    case PM_CLASS_NODE: {
      const pm_class_node_t *n = (const pm_class_node_t *)node;
      if (n->constant_path) pm_visit_node(n->constant_path, sp_load_sites_visit, w);
      if (n->superclass) pm_visit_node(n->superclass, sp_load_sites_visit, w);
      int is_bo = sp_pre_pm_is_basic_object(w->parser, n->superclass);
      w->in_bo += is_bo;
      if (n->body) pm_visit_node(n->body, sp_load_sites_visit, w);
      w->in_bo -= is_bo;
      return false;
    }
    case PM_IN_NODE: {
      const pm_in_node_t *n = (const pm_in_node_t *)node;
      const SpPreFileCtx *saved = w->pre;
      w->pre = NULL;
      if (n->pattern) pm_visit_node(n->pattern, sp_load_sites_visit, w);
      w->pre = saved;
      if (n->statements) pm_visit_node((const pm_node_t *)n->statements, sp_load_sites_visit, w);
      return false;
    }
    case PM_IF_NODE: case PM_UNLESS_NODE: {
      if (w->pre) {
        int is_unless = PM_NODE_TYPE(node) == PM_UNLESS_NODE;
        const pm_node_t *pred = is_unless ? ((const pm_unless_node_t *)node)->predicate : ((const pm_if_node_t *)node)->predicate;
        const pm_node_t *body = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->statements
                                          : (const pm_node_t *)((const pm_if_node_t *)node)->statements;
        const pm_node_t *other = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->else_clause
                                           : ((const pm_if_node_t *)node)->subsequent;
        int tr = sp_pre_pm_truth(w->parser, pred, w->pre, !w->in_bo, 0);
        if (tr >= 0) {
          const pm_node_t *live = (is_unless ? !tr : tr) ? body : other;
          if (live) pm_visit_node(live, sp_load_sites_visit, w);
          return false;
        }
      }
      return true;
    }
    case PM_CALL_NODE: id = ((const pm_call_node_t *)node)->name; call = 1; break;
    case PM_GLOBAL_VARIABLE_READ_NODE: id = ((const pm_global_variable_read_node_t *)node)->name; break;
    case PM_GLOBAL_VARIABLE_WRITE_NODE: id = ((const pm_global_variable_write_node_t *)node)->name; break;
    case PM_GLOBAL_VARIABLE_TARGET_NODE: id = ((const pm_global_variable_target_node_t *)node)->name; break;
    case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: id = ((const pm_global_variable_operator_write_node_t *)node)->name; break;
    case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: id = ((const pm_global_variable_or_write_node_t *)node)->name; break;
    case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: id = ((const pm_global_variable_and_write_node_t *)node)->name; break;
    default: return true;
  }
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&w->parser->constant_pool, id);
  char name[32];   /* (every name asked below is shorter) */
  if (nm->length >= sizeof name) return true;
  memcpy(name, nm->start, nm->length);
  name[nm->length] = '\0';
  if (!call) {
    if (is_load_path_global(name) || is_loaded_features_global(name)) g_require_unread = 1;
  }
  else if (is_load_name(name) || is_autoload_name(name)) g_require_unread = 1;
  else if (w->count && is_require_name(name)) sp_req_written++;
  return !g_require_unread;
}

/* Read the tree of a file (or of the text a macro wrote): set
   g_require_unread when it calls load or autoload, or reads or writes the
   load path or the loaded features, and with `count` add its require and
   require_relative calls to sp_req_written. A name the program computes is
   not found here: the analysis reads a `send(:load)` or a `method(:load)` as
   a name given by value. Once the flag is set the count is moot. */
static void sp_load_sites_note(const pm_parser_t *parser, const pm_node_t *root, int count, const SpPreFileCtx *pre) {
  SpLoadSites w = { parser, count, pre, 0 };
  if (!g_require_unread && root) pm_visit_node(root, sp_load_sites_visit, &w);
}

/* ---- the directories a program puts on its load path ----
 *
 * `$LOAD_PATH.unshift __dir__`, `$: << File.expand_path("lib", __dir__)` as a
 * statement of a file's top level: a directory the evaluator knows becomes a
 * root of the load path, as one from -I is (sp_feature_roots), for the
 * requires that run after the statement. A write under a condition, in a
 * method or in a block is none: the compiler does not follow whether or
 * when it runs, and a file it would name is not found. */

/* Does the evaluator know the load-path argument `arg` of the file in `dir`
   (a path)? 0 for a splat or a computed value. *real: the real directory it
   names, NULL when there is none. */
static int sp_load_dir_arg(const pm_parser_t *pp, const pm_node_t *arg, const char *dir, char **real) {
  PevCtx ev;
  memset(&ev, 0, sizeof ev);
  ev.dir = dir;
  const pm_parser_t *saved = g_parser;   /* (the evaluator reads names from it) */
  g_parser = pp;
  Mv v;
  int ok = pev_eval(&ev, (pm_node_t *)arg, &v) && v.k == MV_STR && v.s[0] == '/';
  g_parser = saved;
  struct stat st;
  *real = ok ? realpath(v.s, NULL) : NULL;
  if (*real && !(stat(*real, &st) == 0 && S_ISDIR(st.st_mode))) { free(*real); *real = NULL; }
  return ok;
}

/* The directory of `file`, real, or NULL */
static char *sp_file_real_dir(const char *file) {
  char *real = file ? realpath(file, NULL) : NULL, *sl = real ? strrchr(real, '/') : NULL;
  if (!sl) { free(real); return NULL; }
  *sl = 0;
  return real;
}

/* Real directory `dir` put on the load path the way the program's write
   puts it: at the front (`first`: unshift, prepend), where the search finds
   it first even if it was on the path already, as CRuby's does through the
   new entry; or at the end, where one already on the path changes nothing. */
static void sp_feature_root_put(const char *dir, int first) {
  for (int i = 0; i < sp_feature_roots_n; i++) {
    char *r = realpath(sp_feature_roots[i], NULL);
    int same = r && strcmp(r, dir) == 0;
    free(r);
    if (!same) continue;
    if (!first) return;
    free(sp_feature_roots[i]);
    memmove(sp_feature_roots + i, sp_feature_roots + i + 1, sizeof *sp_feature_roots * (size_t)(sp_feature_roots_n - i - 1));
    sp_feature_roots_n--;
    break;
  }
  sp_feature_root_at(dir, first);
}

/* Constant `id`'s name copied to `buf` (0 when it does not fit), for the
   name layer's predicates. */
static int sp_name_buf(const pm_parser_t *pp, pm_constant_id_t id, char *buf, size_t cap) {
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&pp->constant_pool, id);
  if (nm->length >= cap) return 0;
  memcpy(buf, nm->start, nm->length);
  buf[nm->length] = '\0';
  return 1;
}

/* Top-level statement `stmt` of the file in `dir`, a write to the load path
   (a call, or a chain of calls, on the load path's global): the directories
   it adds are roots from here on, in the order CRuby's path takes them.
   Nothing is added unless the evaluator knows every argument of every link
   of the chain (it does not know a splat or a computed value): the known
   ones alone would put a file's directory in the wrong place. A chain
   (`$: << a << b`) adds its receiver's directories first. 1 when `stmt` is
   such a write; the directories are added when `apply`, and only counted as
   known otherwise, so a chain is added whole or not at all. */
static int sp_load_path_stmt(const pm_parser_t *pp, const pm_node_t *stmt, const char *dir, int apply) {
  if (PM_NODE_TYPE(stmt) != PM_CALL_NODE) return 0;
  const pm_call_node_t *c = (const pm_call_node_t *)stmt;
  char name[16];
  if (!c->receiver || c->block || !c->arguments || !sp_name_buf(pp, c->name, name, sizeof name) || !is_load_path_write(name)) return 0;
  if (PM_NODE_TYPE(c->receiver) == PM_GLOBAL_VARIABLE_READ_NODE) {
    char gv[16];
    if (!sp_name_buf(pp, ((const pm_global_variable_read_node_t *)c->receiver)->name, gv, sizeof gv) || !is_load_path_global(gv)) return 0;
  }
  else if (!sp_load_path_stmt(pp, c->receiver, dir, apply)) return 0;
  const pm_node_list_t *args = &c->arguments->arguments;
  int concat = is_concat_name(name), first = is_prepend_alias(name);
  if (is_shovel_name(name) && args->size != 1) return 0;   /* (`<<` takes one argument: more raise ArgumentError) */
  /* the directory nodes in argument order: concat's are its array literals' elements */
  size_t total = 0, n = 0;
  for (size_t i = 0; i < args->size; i++)
    total += concat && PM_NODE_TYPE(args->nodes[i]) == PM_ARRAY_NODE ? ((const pm_array_node_t *)args->nodes[i])->elements.size : 1;
  const pm_node_t **nodes = malloc(sizeof *nodes * (total + 1));
  char **real = calloc(total + 1, sizeof *real);
  int known = 1;
  if (!nodes || !real) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (size_t i = 0; i < args->size; i++) {
    const pm_node_t *arg = args->nodes[i];
    if (!concat) nodes[n++] = arg;
    else if (PM_NODE_TYPE(arg) != PM_ARRAY_NODE) known = 0;
    else
      for (size_t k = 0; k < ((const pm_array_node_t *)arg)->elements.size; k++)
        nodes[n++] = ((const pm_array_node_t *)arg)->elements.nodes[k];
  }
  for (size_t i = 0; known && i < n; i++) known = sp_load_dir_arg(pp, nodes[i], dir, &real[i]);
  /* unshift(a, b) leaves [a, b, *old]: the last argument goes in first */
  for (size_t i = 0; apply && known && i < n; i++) {
    const char *d = real[first ? n - 1 - i : i];
    if (d) sp_feature_root_put(d, first);
  }
  for (size_t i = 0; i < n; i++) free(real[i]);
  free(real);
  free(nodes);
  return known;
}

/* ---- require_relative calls, read from the file's own tree ----
 *
 * Each `require_relative` of a file is a CallNode of its own parse (#9898):
 * no receiver, or `Kernel.` / `::Kernel.`, no block, one argument that is a
 * single- or double-quoted literal on one line (in parentheses or not). Text
 * a String, a heredoc, `%q()` or `=begin` holds is never one, as it was for
 * the line scanner (sp_require_find), which plain requires keep until each
 * file's tree is flattened by itself. Where the call stands is read from its
 * enclosing nodes: the top-level statement it is in, whether only control
 * flow surrounds it, a modifier `if`/`unless` that is its whole condition,
 * or a value position that needs the file inlined ahead of the statement.
 * All offsets are into the text the tree was parsed from. */
typedef struct {
  size_t kw, end;            /* the call */
  size_t arg, arg_len;       /* the literal's bytes as written, between its quotes */
  char quote;
  int margin;                /* the call starts its line */
  int ml;                    /* bytes after the call that a modifier `if`/`unless` takes, to the line's end */
  int has_modifier;          /* the call is part of a larger statement */
  int control_only;          /* alone on its line, inside nothing but control flow */
  size_t stmt_start;         /* line start of the top-level statement it is in */
  size_t ctl_stmt_start;     /* line start of the innermost statement around it with only control flow above */
  int rel_stmt;              /* (in a rewrite) stmt_start and ctl_stmt_start are offsets in its text */
  int plain;                 /* a plain `require` (the load graph's; the plain pass still scans) */
  int line;                  /* the call's line in its file */
  int deferred;              /* the require may not run where it stands (not sp_req_always_runs) */
} SpReqSite;

/* A rewrite the tree asks for: the text [a, b) becomes `text`, and the
   require_relative calls it writes are `sites`, their kw, arg and end
   offsets in `text` (sp_req_rewrite_note). */
typedef struct {
  size_t a, b;
  char *text;
  SpReqSite *sites;
  int nsites;
} SpReqEdit;

/* A file's first `return` of its own (sp_req_always_runs): where it starts, NULL for none, once known. */
typedef struct { const uint8_t *at; int known; } SpFileReturn;
typedef struct {
  const pm_parser_t *parser;
  const char *text;
  const char *path;
  const SpNodeList *skip;
  const pm_node_t **stack;   /* the visited node's ancestors, the ProgramNode first */
  int depth, stack_cap;
  SpReqSite *v;
  int n, cap;
  const char *dir;           /* for the rewrites: the file's directory, NULL for none */
  int is_main;               /* the entry file (an autoload becomes its require in place) */
  SpReqEdit *ed;
  int ned, edcap;
  SpReqEdit tail;            /* a required file's autoloads, written at its end */
  SpFileReturn ret;          /* its first return (sp_req_always_runs) */
} SpReqSiteScan;

static size_t sp_line_start_of(const char *text, size_t at) {
  while (at > 0 && text[at - 1] != '\n') at--;
  return at;
}

/* `node` (an if/unless/while/until) written after the statement it guards */
static int sp_node_is_modifier(const pm_node_t *node) {
  const pm_location_t *kw = NULL;
  const pm_statements_node_t *body = NULL;
  switch (PM_NODE_TYPE(node)) {
  case PM_IF_NODE: kw = &((const pm_if_node_t *)node)->if_keyword_loc; body = ((const pm_if_node_t *)node)->statements; break;
  case PM_UNLESS_NODE: kw = &((const pm_unless_node_t *)node)->keyword_loc; body = ((const pm_unless_node_t *)node)->statements; break;
  case PM_WHILE_NODE: kw = &((const pm_while_node_t *)node)->keyword_loc; body = ((const pm_while_node_t *)node)->statements; break;
  case PM_UNTIL_NODE: kw = &((const pm_until_node_t *)node)->keyword_loc; body = ((const pm_until_node_t *)node)->statements; break;
  default: return 0;
  }
  return kw->start && body && kw->start > body->base.location.start;
}

static int sp_node_is_control(const pm_node_t *node) {
  switch (PM_NODE_TYPE(node)) {
  case PM_STATEMENTS_NODE: case PM_IF_NODE: case PM_UNLESS_NODE: case PM_ELSE_NODE:
  case PM_CASE_NODE: case PM_WHEN_NODE: case PM_BEGIN_NODE: case PM_RESCUE_NODE:
  case PM_ENSURE_NODE: case PM_WHILE_NODE: case PM_UNTIL_NODE:
    return 1;
  default:
    return 0;
  }
}

/* Can the bytes of a path literal be written back into a spliced
   `require_relative <path>` as they are? A backslash is an escape in either
   quote; a `#` (which `#{`, `#@` and `#$` would open an interpolation in
   double quotes, as in `aep/index#1`, #10724) or a `"` puts the path in
   single quotes (sp_req_path_quote), where a `'` could not stand. */
static int sp_req_path_ok(const char *p, size_t n) {
  return !memchr(p, '\\', n) && !(memchr(p, '\'', n) && (memchr(p, '#', n) || memchr(p, '"', n)));
}
/* ...and the quote the splice writes it in */
static char sp_req_path_quote(const char *p, size_t n) {
  return memchr(p, '#', n) || memchr(p, '"', n) ? '\'' : '"';
}

/* a single- or double-quoted literal on one line */
static int sp_req_quoted_line(const pm_location_t *open, const pm_location_t *close) {
  return open && close && open->start && close->start && (*open->start == '"' || *open->start == '\'') &&
         open->end - open->start == 1 && !memchr(open->start, '\n', (size_t)(close->end - open->start));
}

/* Where a file's first `return` of its own (not in a method, block or lambda) starts, once for the file's tree: every
   require of the file asks it (sp_req_always_runs). at is NULL for none; known once the tree was visited. */
static bool sp_first_return_visit(const pm_node_t *node, void *data) {
  int t = PM_NODE_TYPE(node);
  const uint8_t **at = data;
  if (t == PM_DEF_NODE || t == PM_BLOCK_NODE || t == PM_LAMBDA_NODE) return false;
  if (t == PM_RETURN_NODE && (!*at || node->location.start < *at)) *at = node->location.start;
  return true;
}

/* Does the require `node` (below the ancestors stack[0..depth), the file's ProgramNode first) always run where its
   file's statements run? The load graph's LoadNode carries its negation as req_deferred, written here and nowhere
   else. It does when both hold:
   - the require is a statement of its own, and every ancestor is the file's top-level statement list, a class,
     module or singleton class body, or the body of a `begin` that has no rescue clause (with one, an earlier
     statement's raise can skip it; without, the raise ends the program, and nothing observes the difference);
   - no `return` of the file's own (conditional or not) starts before it, in an earlier statement of the top level or of
     an enclosing begin, which ends the load before the require.
   Every other placement (a method, a block, a lambda, a branch, a loop, a condition or short-circuit in expression
   position, a rescue clause, an argument) is deferred; and a file required from a deferred one is deferred
   (prog_stamp_deferred). */
static int sp_req_always_runs(const pm_node_t *const *stack, int depth, const pm_node_t *node, SpFileReturn *ret) {
  if (depth < 1 || PM_NODE_TYPE(stack[0]) != PM_PROGRAM_NODE) return 0;
  pm_node_type_t pt = PM_NODE_TYPE(stack[depth - 1]);
  if (pt != PM_STATEMENTS_NODE && pt != PM_PROGRAM_NODE) return 0;
  for (int i = 0; i < depth; i++) {
    const pm_node_t *a = stack[i], *child = i + 1 < depth ? stack[i + 1] : node;
    switch (PM_NODE_TYPE(a)) {
    case PM_PROGRAM_NODE: case PM_STATEMENTS_NODE: break;
    case PM_BEGIN_NODE:
      if (((const pm_begin_node_t *)a)->rescue_clause || child != (const pm_node_t *)((const pm_begin_node_t *)a)->statements) return 0;
      break;
    case PM_CLASS_NODE: if (child != ((const pm_class_node_t *)a)->body) return 0; break;
    case PM_MODULE_NODE: if (child != ((const pm_module_node_t *)a)->body) return 0; break;
    case PM_SINGLETON_CLASS_NODE: if (child != ((const pm_singleton_class_node_t *)a)->body) return 0; break;
    default: return 0;
    }
  }
  /* (every ancestor above is a list, a class or module body or a begin body, and a `return` in a class body is a
     syntax error: one that starts before the require is in an earlier statement of a list on the way to it) */
  if (!ret->known) { pm_visit_node(stack[0], sp_first_return_visit, &ret->at); ret->known = 1; }
  if (ret->at && ret->at < node->location.start) return 0;
  return 1;
}

/* Where `node` (a require, or the statement a rewrite puts requires in
   place of) stands: the facts a site's splice is chosen by, read from its
   ancestors on `s`'s stack. `st->kw` and `st->end` are its span. */
static void sp_req_site_place(SpReqSiteScan *s, const pm_node_t *node, SpReqSite *st) {
  const char *t = s->text;
  st->line = (int)pm_newline_list_line(&s->parser->newline_list, node->location.start, s->parser->start_line);
  size_t ls = sp_line_start_of(t, st->kw);
  st->margin = ls == st->kw;
  const pm_node_t *parent = s->depth > 0 ? s->stack[s->depth - 1] : NULL;
  const pm_node_t *grand = s->depth > 1 ? s->stack[s->depth - 2] : NULL;
  int in_stmts = parent && PM_NODE_TYPE(parent) == PM_STATEMENTS_NODE;
  if (in_stmts && grand && sp_node_is_modifier(grand)) {
    /* the modifier's condition, then blanks and a comment, to the line's end */
    size_t q = (size_t)((const char *)grand->location.end - t), e = q;
    if ((PM_NODE_TYPE(grand) == PM_IF_NODE || PM_NODE_TYPE(grand) == PM_UNLESS_NODE) &&
        ((const pm_statements_node_t *)parent)->body.size == 1 && !memchr(t + st->end, '\n', q - st->end)) {
      while (t[e] == ' ' || t[e] == '\t') e++;
      if (t[e] == '#') while (t[e] && t[e] != '\n' && t[e] != '\r') e++;
      if (t[e] == '\n' || t[e] == '\r' || !t[e]) st->ml = (int)(e - st->end);
    }
    st->has_modifier = 1;
  }
  else if (!in_stmts) st->has_modifier = 1;
  if (in_stmts && (!st->has_modifier || st->ml) && s->depth > 2) {
    int alone = 1;
    for (size_t i = ls; i < st->kw; i++) if (t[i] != ' ' && t[i] != '\t') alone = 0;
    st->control_only = alone;
    for (int i = 1; st->control_only && i < s->depth; i++)
      if (!sp_node_is_control(s->stack[i])) st->control_only = 0;
  }
  st->deferred = !sp_req_always_runs(s->stack, s->depth, node, &s->ret);
  const pm_node_t *top = s->depth > 2 ? s->stack[2] : node;
  st->stmt_start = sp_line_start_of(t, (size_t)((const char *)top->location.start - t));
  /* the innermost statement (the call or an ancestor right under a list of
     statements) with nothing but control flow above it */
  const pm_node_t *ctl = top;
  for (int k = 2, ok = 1; k <= s->depth && ok; k++) {
    if (PM_NODE_TYPE(s->stack[k - 1]) == PM_STATEMENTS_NODE)
      ctl = k == s->depth ? node : s->stack[k];
    ok = k < s->depth && sp_node_is_control(s->stack[k]);
  }
  st->ctl_stmt_start = sp_line_start_of(t, (size_t)((const char *)ctl->location.start - t));
}

static int sp_req_dir_relative(SpReqSiteScan *s, const pm_call_node_t *call, const pm_node_t *arg);
static int sp_req_unresolved(SpReqSiteScan *s, const pm_call_node_t *call, const pm_node_t *arg);

static void sp_req_site_note(SpReqSiteScan *s, const pm_call_node_t *call) {
  const pm_parser_t *pp = s->parser;
  int plain = sp_pm_name_is(pp, call->name, "require");
  if ((!plain && !sp_pm_name_is(pp, call->name, "require_relative")) || !call->arguments) return;
  if (call->receiver && (!sp_call_receiver_is_kernel(pp, call->receiver) ||
                         !call->call_operator_loc.start || *call->call_operator_loc.start != '.')) return;
  for (int i = 0; i < s->skip->n; i++) if (s->skip->v[i] == (const pm_node_t *)call) return;
  const pm_node_t *arg = call->arguments->arguments.nodes[0];
  if (!plain && sp_req_dir_relative(s, call, arg)) return;
  const pm_location_t *open = NULL, *close = NULL;
  const pm_node_t *lit = arg;
  if (PM_NODE_TYPE(lit) == PM_PARENTHESES_NODE) {
    const pm_node_t *body = ((const pm_parentheses_node_t *)lit)->body;
    lit = body && PM_NODE_TYPE(body) == PM_STATEMENTS_NODE &&
          ((const pm_statements_node_t *)body)->body.size == 1 ? ((const pm_statements_node_t *)body)->body.nodes[0] : NULL;
  }
  if (lit && PM_NODE_TYPE(lit) == PM_STRING_NODE) {
    open = &((const pm_string_node_t *)lit)->opening_loc; close = &((const pm_string_node_t *)lit)->closing_loc;
  }
  else if (lit && PM_NODE_TYPE(lit) == PM_INTERPOLATED_STRING_NODE) {
    open = &((const pm_interpolated_string_node_t *)lit)->opening_loc;
    close = &((const pm_interpolated_string_node_t *)lit)->closing_loc;
  }
  int one_literal = !call->block && call->arguments->arguments.size == 1 && sp_req_quoted_line(open, close);
  int head_only = 0;   /* the literal is only the head of the argument */
  if (!one_literal) {
    /* A call written without parentheses whose first argument starts with
       a quoted literal was taken by the line scanner for a require of that
       literal, the rest left applied to the call's value
       (`require_relative "a".freeze`), and so it still is. A binary
       operator on the literal (`"a" + ""`) or adjacent literals
       (`"a" "b"`, two parts to Prism) only reached a refusal that way;
       they are refused here in so many words. Anything else (a variable,
       a call in parentheses) stays a call at run time. */
    if (call->opening_loc.start) return;
    int binary = 0;
    lit = arg;
    while (PM_NODE_TYPE(lit) == PM_CALL_NODE && ((const pm_call_node_t *)lit)->receiver &&
           ((const pm_call_node_t *)lit)->receiver->location.start == lit->location.start) {
      const pm_call_node_t *c = (const pm_call_node_t *)lit;
      if (!c->call_operator_loc.start && !c->opening_loc.start && c->arguments && c->arguments->arguments.size == 1)
        binary = 1;   /* `lit + x`, not `lit.m(x)` or `lit[x]` */
      lit = c->receiver;
    }
    int adjacent = PM_NODE_TYPE(lit) == PM_INTERPOLATED_STRING_NODE &&
                   !((const pm_interpolated_string_node_t *)lit)->opening_loc.start;
    if ((adjacent || binary) && plain) return;
    if (adjacent || binary) {
      /* a literal head is one the user meant as the path; any other head is a
         path only the run time knows (those of the file's own directory were
         spliced above): it raises LoadError when reached */
      int lit_head = adjacent || PM_NODE_TYPE(lit) == PM_STRING_NODE || PM_NODE_TYPE(lit) == PM_INTERPOLATED_STRING_NODE;
      if (!lit_head && sp_req_unresolved(s, call, arg)) return;
      fprintf(stderr, "spinel: %s:%d: require_relative needs its path as one string literal\n", s->path,
              (int)pm_newline_list_line(&pp->newline_list, call->base.location.start, pp->start_line));
      exit(1);
    }
    open = close = NULL;
    if (PM_NODE_TYPE(lit) == PM_STRING_NODE) {
      open = &((const pm_string_node_t *)lit)->opening_loc; close = &((const pm_string_node_t *)lit)->closing_loc;
    }
    else if (PM_NODE_TYPE(lit) == PM_INTERPOLATED_STRING_NODE) {
      open = &((const pm_interpolated_string_node_t *)lit)->opening_loc;
      close = &((const pm_interpolated_string_node_t *)lit)->closing_loc;
    }
    if (!sp_req_quoted_line(open, close) || open->start != arg->location.start) return;
    head_only = 1;
  }
  const char *t = s->text;
  SpReqSite st;
  memset(&st, 0, sizeof st);
  st.kw = (size_t)((const char *)call->base.location.start - t);
  st.end = (size_t)((const char *)(head_only ? close->end : call->base.location.end) - t);
  st.arg = (size_t)((const char *)open->end - t);
  st.arg_len = (size_t)(close->start - open->end);
  st.quote = (char)*open->start;
  st.plain = plain;
  sp_req_site_place(s, (const pm_node_t *)call, &st);
  if (head_only) { st.ml = 0; st.has_modifier = 1; }   /* the rest is applied to the value */
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->v = realloc(s->v, sizeof *s->v * (size_t)s->cap);
    if (!s->v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  s->v[s->n++] = st;
}

/* ---- computed requires and autoloads, read from the tree ----
 *
 * `require File.join(Archive::LIBPATH, "ffi-libarchive", "archive")`,
 * `require ZMQ.libpath(["ffi-rzmq", file])` inside `%w(util context).each do
 * |file|`: the name is computed, but from literals that spell a path under the
 * requiring file's own directory (the Mr Bones gem template, among others;
 * #5700). Such a require becomes `require_relative` of the joined literals
 * when that file exists; a `%w(...).each` loop around it is unrolled, one
 * require per line of the loop and the rest on its last line, the lines
 * padded so the ones after it keep their numbers. The require must be a
 * statement on its own line (only a comment may follow). `__FILE__` may
 * only stand under `File.dirname`, or as `File.expand_path("../x",
 * __FILE__)`, whose literal is relative to the file.
 *
 * `autoload :Name, "path"` with a literal path (#5696): the whole program is
 * compiled, so the file is loaded eagerly. In the entry file the call
 * becomes the require in place; in a required file it answers nil there,
 * and the requires go at the end of the file -- after the module body the
 * autoload sits in, which the loaded file usually reopens. A path that
 * starts with `#{__dir__}/` is literal enough: it is a require_relative, and
 * so is `File.expand_path('path', __dir__)`, and a file beside this one is
 * one too (lib/foo.rb autoloading "foo/bar" is lib/foo/bar.rb, the gem
 * layout). In a required file the name may be a local (`%w[A B].each { |n|
 * autoload n, ... }`): loaded eagerly, the file is the same whatever name it
 * was asked for. A receiver form (`Mod.autoload`) or any other computed path
 * is left as it was.
 *
 * Both are found on the file's tree (#9898), so text in a String, a heredoc
 * or a comment is never one, and each rewrite is a text edit plus the
 * require_relative sites it writes. A plain `require` an autoload writes is
 * left to the plain pass. */
static int sp_path_exists(const char *p) { FILE *f = fopen(p, "r"); if (f) { fclose(f); return 1; } return 0; }

static int sp_autoload_beside(const char *dir, const char *path, size_t plen) {
  char fp[1400];
  snprintf(fp, sizeof fp, "%s/%.*s%s", dir, (int)plen, path,
           (plen >= 3 && strncmp(path + plen - 3, ".rb", 3) == 0) ? "" : ".rb");
  return sp_path_exists(fp);
}

typedef struct {
  const pm_parser_t *parser;
  const char *var, *val;     /* a loop's block parameter and the word it stands for */
  char *out; size_t o, cap;
  int n, bad;
  int up;                    /* `File.expand_path(lit, __FILE__)`: lit's first `..` is the file's own dir */
} SpJoin;

/* `File.dirname(__FILE__)`: the requiring file's directory, as `__dir__` is. */
static int sp_join_is_file_dir(const pm_parser_t *pp, const pm_node_t *n) {
  if (PM_NODE_TYPE(n) != PM_CALL_NODE) return 0;
  const pm_call_node_t *c = (const pm_call_node_t *)n;
  return sp_pm_name_is(pp, c->name, "dirname") && c->receiver &&
         PM_NODE_TYPE(c->receiver) == PM_CONSTANT_READ_NODE &&
         sp_pm_name_is(pp, ((const pm_constant_read_node_t *)c->receiver)->name, "File") &&
         c->arguments && c->arguments->arguments.size == 1 &&
         PM_NODE_TYPE(c->arguments->arguments.nodes[0]) == PM_SOURCE_FILE_NODE;
}

static bool sp_join_visit(const pm_node_t *node, void *data);

/* The quoted literals of a computed require's argument, joined by '/', with
   the loop's parameter standing for its word ('.' and '/' pieces dropped).
   `__dir__` and `File.dirname(__FILE__)` stand for the requiring file's
   directory, the one the joined path is taken from. */
static bool sp_join_visit(const pm_node_t *node, void *data) {
  SpJoin *j = data;
  const char *piece = NULL; size_t plen = 0;
  switch (PM_NODE_TYPE(node)) {
  case PM_CALL_NODE: {
    if (sp_join_is_file_dir(j->parser, node)) return false;
    const pm_call_node_t *c = (const pm_call_node_t *)node;
    if (sp_pm_name_is(j->parser, c->name, "expand_path") && c->arguments && c->arguments->arguments.size == 2 &&
        PM_NODE_TYPE(c->arguments->arguments.nodes[1]) == PM_SOURCE_FILE_NODE) {
      if (j->n) { j->bad = 1; return false; }
      j->up = 1;
      pm_visit_node(c->arguments->arguments.nodes[0], sp_join_visit, j);
      return false;
    }
    return true;
  }
  case PM_STRING_NODE: {
    const pm_string_node_t *sn = (const pm_string_node_t *)node;
    if (!sn->opening_loc.start || (*sn->opening_loc.start != '"' && *sn->opening_loc.start != '\'')) return false;
    piece = (const char *)sn->content_loc.start; plen = (size_t)(sn->content_loc.end - sn->content_loc.start);
    if (!sp_req_path_ok(piece, plen)) j->bad = 1;
    break;
  }
  case PM_INTERPOLATED_STRING_NODE: case PM_SOURCE_FILE_NODE:
    j->bad = 1; return false;
  case PM_LOCAL_VARIABLE_READ_NODE:
    if (j->var && sp_pm_name_is(j->parser, ((const pm_local_variable_read_node_t *)node)->name, j->var)) {
      piece = j->val; plen = strlen(j->val);
    }
    break;
  default:
    return true;
  }
  while (plen && piece[0] == '/') { piece++; plen--; }   /* `File.dirname(__FILE__) + "/x"` */
  if (!piece || !plen || (plen == 1 && piece[0] == '.')) return false;
  if (j->o + plen + 2 >= j->cap) { j->bad = 1; return false; }
  if (j->n++) j->out[j->o++] = '/';
  memcpy(j->out + j->o, piece, plen);
  j->o += plen;
  j->out[j->o] = 0;
  return false;
}

/* `call` as a computed require naming a file beside this one: its joined
   path in `out`. Only blanks may stand between `lo` and the call, and only
   blanks (and, with `comment`, a comment) between the call and `hi`. */
static int sp_req_computed(const SpReqSiteScan *s, const pm_call_node_t *c, const char *var, const char *val,
                           const char *lo, const char *hi, int comment, char *out, size_t cap) {
  if (!sp_pm_name_is(s->parser, c->name, "require") || c->receiver || c->block || !c->arguments ||
      c->arguments->arguments.size != 1) return 0;
  const pm_node_t *arg = c->arguments->arguments.nodes[0];
  if (PM_NODE_TYPE(arg) == PM_STRING_NODE || PM_NODE_TYPE(arg) == PM_INTERPOLATED_STRING_NODE) return 0;
  const char *cs = (const char *)c->base.location.start, *ce = (const char *)c->base.location.end;
  if (memchr(cs, '\n', (size_t)(ce - cs))) return 0;
  for (const char *q = lo; q < cs; q++) if (*q != ' ' && *q != '\t') return 0;
  for (const char *q = ce; q < hi && *q != '\n'; q++) {
    if (comment && *q == '#') break;
    if (*q != ' ' && *q != '\t' && *q != '\r') return 0;
  }
  SpJoin j = { s->parser, var, val, out, 0, cap, 0, 0, 0 };
  out[0] = 0;
  pm_visit_node(arg, sp_join_visit, &j);
  if (j.bad || !j.n) return 0;
  if (j.up) {                              /* relative to the file: `../x` is `x` beside it */
    if (strncmp(out, "../", 3) != 0) return 0;
    memmove(out, out + 3, j.o - 3 + 1); j.o -= 3;
  }
  if (j.o > 3 && strcmp(out + j.o - 3, ".rb") == 0) out[j.o - 3] = 0;   /* require_relative adds it back */
  char full[1100];
  snprintf(full, sizeof full, "%s/%s.rb", s->dir, out);
  return sp_path_exists(full);
}

static SpReqEdit *sp_req_edit_new(SpReqSiteScan *s, size_t a, size_t b) {
  if (s->ned == s->edcap) {
    s->edcap = s->edcap ? s->edcap * 2 : 4;
    s->ed = realloc(s->ed, sizeof *s->ed * (size_t)s->edcap);
    if (!s->ed) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  SpReqEdit *e = &s->ed[s->ned++];
  memset(e, 0, sizeof *e);
  e->a = a; e->b = b;
  return e;
}

/* Append `str` to the edit's text; with `place`, it writes the call
   `require_relative "<path>"` (at `kw` in the text) placed as `place` says. */
static void sp_req_edit_put(SpReqEdit *e, const char *str, size_t kw, size_t plen, const SpReqSite *place) {
  size_t have = e->text ? strlen(e->text) : 0, add = strlen(str);
  e->text = realloc(e->text, have + add + 1);
  if (!e->text) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(e->text + have, str, add + 1);
  if (!place) return;
  e->sites = realloc(e->sites, sizeof *e->sites * (size_t)(e->nsites + 1));
  if (!e->sites) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  SpReqSite *st = &e->sites[e->nsites++];
  *st = *place;
  st->kw = kw;
  st->arg = kw + (st->plain ? 9 : 18);     /* past `require "` or `require_relative "` */
  st->arg_len = plen;
  st->end = st->arg + plen + 1;
  st->quote = st->arg >= 1 && e->text[st->arg - 1] == '\'' ? '\'' : '"';   /* single for a path with a `#` */
}

/* `__dir__` or `File.dirname(__FILE__)`: the requiring file's directory. */
static int sp_req_is_dir(const pm_parser_t *pp, const pm_node_t *n) {
  if (PM_NODE_TYPE(n) != PM_CALL_NODE) return 0;
  const pm_call_node_t *c = (const pm_call_node_t *)n;
  if (sp_pm_name_is(pp, c->name, "__dir__")) return !c->receiver && !c->arguments;
  return sp_pm_name_is(pp, c->name, "dirname") && c->receiver && PM_NODE_TYPE(c->receiver) == PM_CONSTANT_READ_NODE &&
         sp_pm_name_is(pp, ((const pm_constant_read_node_t *)c->receiver)->name, "File") && c->arguments &&
         c->arguments->arguments.size == 1 && PM_NODE_TYPE(c->arguments->arguments.nodes[0]) == PM_SOURCE_FILE_NODE;
}

/* The bytes of a plain quoted literal without escapes or interpolation. */
static int sp_req_plain(const pm_node_t *n, const char **str, size_t *len) {
  if (PM_NODE_TYPE(n) != PM_STRING_NODE) return 0;
  const pm_string_node_t *sn = (const pm_string_node_t *)n;
  if (!sp_req_quoted_line(&sn->opening_loc, &sn->closing_loc)) return 0;
  *str = (const char *)sn->content_loc.start; *len = (size_t)(sn->content_loc.end - sn->content_loc.start);
  return sp_req_path_ok(*str, *len);
}

/* `dir + "a" + "b"...`: the literals' bytes after the directory, appended to
   `out` (left-associated, as Ruby reads it). */
static int sp_req_dir_sum(const pm_parser_t *pp, const pm_node_t *n, char *out, size_t cap, size_t *o) {
  const char *str; size_t len;
  if (sp_req_is_dir(pp, n)) return 1;
  if (PM_NODE_TYPE(n) != PM_CALL_NODE) return 0;
  const pm_call_node_t *c = (const pm_call_node_t *)n;
  if (!sp_pm_name_is(pp, c->name, "+") || !c->receiver || c->call_operator_loc.start || c->opening_loc.start ||
      !c->arguments || c->arguments->arguments.size != 1 || !sp_req_plain(c->arguments->arguments.nodes[0], &str, &len) ||
      !sp_req_dir_sum(pp, c->receiver, out, cap, o) || *o + len + 1 >= cap) return 0;
  memcpy(out + *o, str, len); *o += len;
  return 1;
}

/* The call written as `require_relative "<path>"` where it stands (a path with
   `#{` raises LoadError at run time, as any computed one). */
/* ...written in quote `q`: the one the path takes (sp_req_path_quote), or
   `"` for the `#{...}` an unresolved path is spelled as */
static int sp_req_splice_relative(SpReqSiteScan *s, const pm_call_node_t *call, const char *path, char q) {
  const char *t = s->text;
  size_t cs = (size_t)((const char *)call->base.location.start - t), ce = (size_t)((const char *)call->base.location.end - t);
  if (memchr(t + cs, '\n', ce - cs)) return 0;
  SpReqSite place;
  memset(&place, 0, sizeof place);
  place.kw = cs; place.end = ce;
  sp_req_site_place(s, (const pm_node_t *)call, &place);
  size_t plen = strlen(path), cap = plen + 20;
  char *buf = malloc(cap);
  if (!buf) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  snprintf(buf, cap, "require_relative %c%s%c", q, path, q);
  sp_req_edit_put(sp_req_edit_new(s, cs, ce), buf, 0, plen, &place);
  free(buf);
  return 1;
}

/* The path `n` spells after the file's own directory, in `out` with its length:
   `dir + "/x"` (a chain of literals too), `"#{dir}/x"`, `File.join(dir, "x", ...)`,
   `File.expand_path("x", dir)`, `dir` being `__dir__` or `File.dirname(__FILE__)`.
   The one fact a require_relative and an autoload take their path from. */
static int sp_req_dir_literal(const pm_parser_t *pp, const pm_node_t *n, char *out, size_t cap, size_t *olen) {
  const char *str; size_t len, o = 0;
  if (PM_NODE_TYPE(n) == PM_INTERPOLATED_STRING_NODE) {             /* "#{__dir__}/x" */
    const pm_interpolated_string_node_t *is = (const pm_interpolated_string_node_t *)n;
    if (!sp_req_quoted_line(&is->opening_loc, &is->closing_loc) || *is->opening_loc.start != '"' || is->parts.size != 2 ||
        PM_NODE_TYPE(is->parts.nodes[0]) != PM_EMBEDDED_STATEMENTS_NODE || PM_NODE_TYPE(is->parts.nodes[1]) != PM_STRING_NODE) return 0;
    const pm_statements_node_t *es = ((const pm_embedded_statements_node_t *)is->parts.nodes[0])->statements;
    const pm_string_node_t *tail = (const pm_string_node_t *)is->parts.nodes[1];
    str = (const char *)tail->content_loc.start; len = (size_t)(tail->content_loc.end - tail->content_loc.start);
    if (!es || es->body.size != 1 || !sp_req_is_dir(pp, es->body.nodes[0]) || len < 2 || *str != '/' || len >= cap ||
        !sp_req_path_ok(str, len)) return 0;
    memcpy(out, str + 1, len - 1); o = len - 1;
  }
  else if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    const pm_call_node_t *a = (const pm_call_node_t *)n;
    const pm_node_t **av = a->arguments ? (const pm_node_t **)a->arguments->arguments.nodes : NULL;
    size_t an = a->arguments ? a->arguments->arguments.size : 0;
    int file = a->receiver && PM_NODE_TYPE(a->receiver) == PM_CONSTANT_READ_NODE &&
               sp_pm_name_is(pp, ((const pm_constant_read_node_t *)a->receiver)->name, "File");
    if (sp_pm_name_is(pp, a->name, "+")) {
      if (!sp_req_dir_sum(pp, n, out, cap, &o) || o < 2 || out[0] != '/') return 0;
      memmove(out, out + 1, o); o--;
    }
    else if (file && sp_pm_name_is(pp, a->name, "join") && an >= 2 && sp_req_is_dir(pp, av[0])) {
      for (size_t i = 1; i < an; i++) {
        if (!sp_req_plain(av[i], &str, &len)) return 0;
        while (len && *str == '/') { str++; len--; }
        if (!len) continue;
        if (o + len + 2 >= cap) return 0;
        if (o) out[o++] = '/';
        memcpy(out + o, str, len); o += len;
      }
    }
    else if (file && sp_pm_name_is(pp, a->name, "expand_path") && an == 2 && sp_req_is_dir(pp, av[1]) &&
             sp_req_plain(av[0], &str, &len) && len && *str != '/' && len < cap) {
      memcpy(out, str, len); o = len;
    }
  }
  out[o] = 0;
  *olen = o;
  return o > 0;
}

/* A require_relative of such a path: written as `require_relative "x"` where
   it stands. 0 for any other argument. */
static int sp_req_dir_relative(SpReqSiteScan *s, const pm_call_node_t *call, const pm_node_t *arg) {
  char path[512];
  size_t len;
  if (!s->dir || call->block || call->arguments->arguments.size != 1 || !sp_req_dir_literal(s->parser, arg, path, sizeof path, &len)) return 0;
  return sp_req_splice_relative(s, call, path, sp_req_path_quote(path, len));
}

/* A computed path Spinel cannot resolve: the call raises LoadError at run
   time, naming the path, as a `"#{x}"` one does. */
static int sp_req_unresolved(SpReqSiteScan *s, const pm_call_node_t *call, const pm_node_t *arg) {
  if (!s->dir || call->block || call->arguments->arguments.size != 1) return 0;
  size_t alen = (size_t)(arg->location.end - arg->location.start), cap = alen + 4;
  char *path = malloc(cap);
  if (!path) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  snprintf(path, cap, "#{%.*s}", (int)alen, (const char *)arg->location.start);
  int r = sp_req_splice_relative(s, call, path, '"');
  free(path);
  return r;
}

/* Only blanks and `;`, then the line's end or a comment, from `p` on. */
static int sp_req_rest_is_blank(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == ';') p++;
  return !*p || *p == '\n' || *p == '\r' || *p == '#';
}

/* A `%w(...).each` loop whose body is one computed require, every word of
   it naming a file: unrolled. */
static int sp_req_loop(SpReqSiteScan *s, const pm_call_node_t *c) {
  const char *t = s->text;
  const pm_constant_t *mn = pm_constant_pool_id_to_constant(&s->parser->constant_pool, c->name);
  if (mn->length < 4 || memcmp(mn->start, "each", 4) || !c->receiver || c->arguments ||
      PM_NODE_TYPE(c->receiver) != PM_ARRAY_NODE || !c->block || PM_NODE_TYPE(c->block) != PM_BLOCK_NODE) return 0;
  const pm_array_node_t *list = (const pm_array_node_t *)c->receiver;
  const pm_block_node_t *blk = (const pm_block_node_t *)c->block;
  const char *ls = (const char *)list->opening_loc.start;
  if (!ls || ls[0] != '%' || ls[1] != 'w' || (ls[2] != '(' && ls[2] != '[' && ls[2] != '{')) return 0;
  if (!blk->parameters || PM_NODE_TYPE(blk->parameters) != PM_BLOCK_PARAMETERS_NODE) return 0;
  const pm_parameters_node_t *pa = ((const pm_block_parameters_node_t *)blk->parameters)->parameters;
  if (!pa || !pa->requireds.size || PM_NODE_TYPE(pa->requireds.nodes[0]) != PM_REQUIRED_PARAMETER_NODE) return 0;
  if (!blk->body || PM_NODE_TYPE(blk->body) != PM_STATEMENTS_NODE ||
      ((const pm_statements_node_t *)blk->body)->body.size != 1 ||
      PM_NODE_TYPE(((const pm_statements_node_t *)blk->body)->body.nodes[0]) != PM_CALL_NODE) return 0;
  const pm_call_node_t *req = (const pm_call_node_t *)((const pm_statements_node_t *)blk->body)->body.nodes[0];
  /* a statement, or the receiver of a call that is one (`end.size`): the
     lines through `end` are what the loop is unrolled into, so that call
     takes no arguments or block, and nothing but a comment follows the
     statement on its last line (`end.size; X = 1` would lose `X = 1`). An
     `if`/`unless` modifier and its condition are part of that statement,
     and stay what the unrolling does with them (they are not kept). */
  const pm_node_t *parent = s->depth ? s->stack[s->depth - 1] : NULL;
  const char *fin = (const char *)c->base.location.end;
  int gi = s->depth - 2, mod = 0;
  if (parent && PM_NODE_TYPE(parent) == PM_CALL_NODE && ((const pm_call_node_t *)parent)->receiver == (const pm_node_t *)c &&
      s->depth > 1) {
    const pm_call_node_t *outer = (const pm_call_node_t *)parent;
    if (outer->arguments || outer->block) return 0;
    fin = (const char *)outer->base.location.end;
    parent = s->stack[s->depth - 2];
    gi = s->depth - 3;
  }
  if (!parent || PM_NODE_TYPE(parent) != PM_STATEMENTS_NODE) return 0;
  if (gi >= 0 && sp_node_is_modifier(s->stack[gi]) &&
      (PM_NODE_TYPE(s->stack[gi]) == PM_IF_NODE || PM_NODE_TYPE(s->stack[gi]) == PM_UNLESS_NODE)) {
    fin = (const char *)s->stack[gi]->location.end;
    mod = 1;   /* its condition may go on past the line, the lines it leaves are harmless */
  }
  size_t w = (size_t)((const char *)c->base.location.start - t), p = sp_line_start_of(t, w);
  for (size_t i = p; i < w; i++) if (t[i] != ' ' && t[i] != '\t') return 0;
  const char *pclose = (const char *)((const pm_block_parameters_node_t *)blk->parameters)->closing_loc.end;
  const char *line_end = strchr(t + w, '\n');
  if (!line_end) line_end = t + strlen(t);
  const char *lo, *hi; size_t after;
  int brace = *blk->opening_loc.start == '{';
  if (!pclose || pclose > line_end) return 0;
  if (brace) {                              /* `%w(a b).each { |v| require ... }` on one line */
    if ((const char *)blk->closing_loc.start > line_end) return 0;
    lo = pclose; hi = (const char *)blk->closing_loc.start;
    after = *line_end ? (size_t)(line_end - t) + 1 : (size_t)(line_end - t);
    if ((fin > line_end && !mod) || !sp_req_rest_is_blank(fin)) return 0;
  }
  else {                                    /* the do line, the require's line, the end line */
    lo = line_end + 1;
    hi = strchr(lo, '\n');
    if (!hi || (const char *)req->base.location.start < lo || (const char *)req->base.location.start > hi) return 0;
    const char *el = hi + 1, *ee = strchr(el, '\n');
    if (!ee) ee = el + strlen(el);
    const char *ek = (const char *)blk->closing_loc.start;
    if (ek < el || ek > ee) return 0;
    for (const char *q = el; q < ek; q++) if (*q != ' ' && *q != '\t') return 0;
    after = *ee ? (size_t)(ee - t) + 1 : (size_t)(ee - t);
    if ((fin > ee && !mod) || !sp_req_rest_is_blank(fin)) return 0;
  }
  char var[64], joined[512];
  const pm_constant_t *vn = pm_constant_pool_id_to_constant(&s->parser->constant_pool,
                                                            ((const pm_required_parameter_node_t *)pa->requireds.nodes[0])->name);
  if (vn->length >= sizeof var) return 0;
  memcpy(var, vn->start, vn->length); var[vn->length] = 0;
  /* every word must name a file, or nothing is unrolled */
  for (size_t i = 0; i < list->elements.size; i++) {
    const pm_node_t *el = list->elements.nodes[i];
    if (PM_NODE_TYPE(el) != PM_STRING_NODE) return 0;
    const pm_string_node_t *sn = (const pm_string_node_t *)el;
    char word[128];
    snprintf(word, sizeof word, "%.*s", (int)(sn->content_loc.end - sn->content_loc.start), (const char *)sn->content_loc.start);
    if (!sp_req_computed(s, req, var, word, lo, hi, !brace, joined, sizeof joined)) return 0;
  }
  if (!list->elements.size) return 0;
  int old_lines = 0;
  for (size_t i = p; i < after; i++) if (t[i] == '\n') old_lines++;
  if (old_lines < 1) old_lines = 1;
  SpReqSite place;
  memset(&place, 0, sizeof place);
  place.kw = w; place.end = (size_t)((const char *)c->base.location.end - t);
  sp_req_site_place(s, (const pm_node_t *)c, &place);
  place.ml = 0; place.has_modifier = 0;
  SpReqEdit *e = sp_req_edit_new(s, p, after);
  for (size_t i = 0; i < list->elements.size; i++) {
    const pm_string_node_t *sn = (const pm_string_node_t *)list->elements.nodes[i];
    char word[128], buf[700];
    snprintf(word, sizeof word, "%.*s", (int)(sn->content_loc.end - sn->content_loc.start), (const char *)sn->content_loc.start);
    sp_req_computed(s, req, var, word, lo, hi, !brace, joined, sizeof joined);
    size_t at = e->text ? strlen(e->text) : 0;
    char q = sp_req_path_quote(joined, strlen(joined));
    if ((int)i < old_lines) {               /* a line of its own, at the loop's indentation */
      if (i) { sp_req_edit_put(e, "\n", 0, 0, NULL); at++; }
      snprintf(buf, sizeof buf, "%.*srequire_relative %c%s%c", (int)(w - p), t + p, q, joined, q);
      sp_req_edit_put(e, buf, at + (w - p), strlen(joined), &place);
    }
    else {                                  /* the rest after `; `: hoisted, ahead of the loop's statement */
      SpReqSite joined_place = place;
      joined_place.margin = joined_place.control_only = joined_place.has_modifier = 0;
      snprintf(buf, sizeof buf, "; require_relative %c%s%c", q, joined, q);
      sp_req_edit_put(e, buf, at + 2, strlen(joined), &joined_place);
    }
  }
  sp_req_edit_put(e, "\n", 0, 0, NULL);
  for (int k = (int)list->elements.size < old_lines ? (int)list->elements.size : old_lines; k < old_lines; k++)
    sp_req_edit_put(e, "\n", 0, 0, NULL);
  return 1;
}

/* `autoload :Name, <literal path>`: the file is required where the call is
   (the entry file) or at the file's end (a required one). */
static void sp_req_autoload(SpReqSiteScan *s, const pm_call_node_t *c) {
  const pm_parser_t *pp = s->parser;
  const char *t = s->text;
  if (!sp_pm_name_is(pp, c->name, "autoload") || c->receiver || !c->arguments || c->arguments->arguments.size != 2) return;
  const pm_node_t *name = c->arguments->arguments.nodes[0], *arg = c->arguments->arguments.nodes[1];
  if (PM_NODE_TYPE(name) == PM_SYMBOL_NODE) {
    const pm_symbol_node_t *sy = (const pm_symbol_node_t *)name;
    if (!sy->opening_loc.start || sy->opening_loc.end - sy->opening_loc.start != 1 || !sy->value_loc.start) return;
  }
  else if (s->is_main || (PM_NODE_TYPE(name) != PM_LOCAL_VARIABLE_READ_NODE &&
                          !(PM_NODE_TYPE(name) == PM_CALL_NODE && PM_NODE_FLAG_P(name, PM_CALL_NODE_FLAGS_VARIABLE_CALL))))
    return;
  const char *fs = NULL; size_t flen = 0; int dir_rel = 0;
  char dl[512];
  if (PM_NODE_TYPE(arg) == PM_STRING_NODE) {                 /* "path" */
    const pm_string_node_t *sn = (const pm_string_node_t *)arg;
    if (!sp_req_quoted_line(&sn->opening_loc, &sn->closing_loc)) return;
    fs = (const char *)sn->content_loc.start; flen = (size_t)(sn->content_loc.end - sn->content_loc.start);
  }
  else if (sp_req_dir_literal(pp, arg, dl, sizeof dl, &flen)) { fs = dl; dir_rel = 1; }   /* "#{__dir__}/path", File.expand_path('path', __dir__) */
  else return;
  if (!sp_req_path_ok(fs, flen)) return;
  const char *kw = (dir_rel || sp_autoload_beside(s->dir, fs, flen)) ? "require_relative" : "require";
  int rel = kw[7] == '_';
  char buf[1400], q = sp_req_path_quote(fs, flen);
  snprintf(buf, sizeof buf, "%s %c%.*s%c", kw, q, (int)flen, fs, q);
  size_t a = (size_t)((const char *)c->base.location.start - t), b = (size_t)((const char *)c->base.location.end - t);
  if (s->is_main) {                        /* the require in place */
    SpReqSite place;
    memset(&place, 0, sizeof place);
    place.kw = a; place.end = b;
    sp_req_site_place(s, (const pm_node_t *)c, &place);
    place.plain = !rel;
    place.deferred = 1;                    /* an autoload's file runs at its constant's first reference, not here */
    sp_req_edit_put(sp_req_edit_new(s, a, b), buf, 0, flen, &place);
  }
  else {                                   /* nil in place, the require at the end */
    sp_req_edit_put(sp_req_edit_new(s, a, b), "nil", 0, 0, NULL);
    size_t at = s->tail.text ? strlen(s->tail.text) : 0;
    SpReqSite place;
    memset(&place, 0, sizeof place);
    place.margin = 1; place.rel_stmt = 1; place.stmt_start = place.ctl_stmt_start = at;
    place.plain = !rel;
    place.deferred = 1;                    /* (as above) */
    place.line = (int)pm_newline_list_line(&pp->newline_list, c->base.location.start, pp->start_line);
    sp_req_edit_put(&s->tail, buf, at, flen, &place);
    sp_req_edit_put(&s->tail, "\n", 0, 0, NULL);
  }
}

/* A computed require, a loop of them or an autoload at `c`: noted as a
   rewrite. 1 when it was an unrolled loop, whose body is the rewrite's. */
static int sp_req_rewrite_note(SpReqSiteScan *s, const pm_call_node_t *c) {
  for (int i = 0; s->skip && i < s->skip->n; i++)
    if (s->skip->v[i] == (const pm_node_t *)c) return 0;
  const char *t = s->text;
  char joined[512];
  if (sp_req_loop(s, c)) return 1;
  if (sp_pm_name_is(s->parser, c->name, "autoload")) { sp_req_autoload(s, c); return 0; }
  const char *cs = (const char *)c->base.location.start;
  const char *lo = t + sp_line_start_of(t, (size_t)(cs - t)), *hi = strchr(cs, '\n');
  if (!hi) hi = cs + strlen(cs);
  if (!sp_req_computed(s, c, NULL, NULL, lo, hi, 1, joined, sizeof joined)) return 0;
  /* the line becomes `require_relative "<joined>"` at the call's indentation */
  size_t p = (size_t)(lo - t), next = *hi ? (size_t)(hi - t) + 1 : (size_t)(hi - t);
  SpReqSite place;
  memset(&place, 0, sizeof place);
  place.kw = (size_t)(cs - t); place.end = (size_t)((const char *)c->base.location.end - t);
  sp_req_site_place(s, (const pm_node_t *)c, &place);
  char buf[700], q = sp_req_path_quote(joined, strlen(joined));
  snprintf(buf, sizeof buf, "%.*srequire_relative %c%s%c\n", (int)(cs - lo), lo, q, joined, q);
  sp_req_edit_put(sp_req_edit_new(s, p, next), buf, (size_t)(cs - lo), strlen(joined), &place);
  return 0;
}

static bool sp_req_site_visit(const pm_node_t *node, void *data) {
  SpReqSiteScan *s = data;
  if (PM_NODE_TYPE(node) == PM_CALL_NODE) {
    sp_req_site_note(s, (const pm_call_node_t *)node);
    if (s->dir && sp_req_rewrite_note(s, (const pm_call_node_t *)node)) return false;
  }
  if (s->depth == s->stack_cap) {
    s->stack_cap = s->stack_cap ? s->stack_cap * 2 : 32;
    s->stack = realloc(s->stack, sizeof *s->stack * (size_t)s->stack_cap);
    if (!s->stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  s->stack[s->depth++] = node;
  pm_visit_child_nodes(node, sp_req_site_visit, data);
  s->depth--;
  return false;
}


/* ---- The load graph (#9898, the load model's part 1) ----
 *
 * The files the program loads, keyed by canonical path, and each require
 * site of each file read, resolved to its target: what a model that runs
 * each file's top level when it is first required (CRuby's) compiles
 * from. Today's splices still drive the program; the graph is built as
 * they read the files, from each file's tree, and --dump-load-graph prints
 * it with the plain pass's own resolutions beside it, so the two can be
 * compared. A literal with interpolation gets an edge to a path known only
 * at run time; a require of any other expression
 * (`require File.join("lib", ARGV[0])`) is not a site yet. Files are the
 * ones named or read; a file only the graph names is not walked yet.
 * builtins/ and the libraries the parser prepends (Set, IO::Buffer) are
 * left out for now. */
enum { SP_LOAD_FILE, SP_LOAD_NATIVE, SP_LOAD_CORE, SP_LOAD_NONE, SP_LOAD_RUNTIME, SP_LOAD_SET };
typedef struct {
  int from, line, plain;  /* the requiring file, the call's line, `require` (not _relative) */
  char *name;             /* the path as written */
  int how, to;            /* SP_LOAD_*; the target file for SP_LOAD_FILE */
  int stmt, top;          /* the top-level statement of `from` the
                             call stands in (a boot library's: below 0, ahead of the
                             entry's first), and 1 when the call is that statement */
  int node;               /* its LoadNode, once flattened (-1 before) */
  int begin_block;        /* ... and 1 when its statement is a `BEGIN { }`, which runs ahead of the file's others */
  int ncand, *cand;       /* SP_LOAD_SET: the files a path computed when it runs may name, ... */
  int open;               /* ... or (open) any file of the program */
  char *pat_pre, *pat_suf; /* ... a plain require's literal head and tail around what
                             only the run time knows (`"x/#{v}/n"`): open narrows to the
                             program's files a path of that shape can name */
  char **autoload, **auto_path; int *auto_file, nauto;
  int per_word;           /* SP_LOAD_SET: the set is one file for each word of the
                             `each` or `each_with_index` block over a literal Array of
                             Strings the call stands in directly, every word naming one
                             (SpPaths.per_word) */
} SpLoadEdge;

/* a require of a file's tree: its edge, the call node it
   was found at, and how many LoadNodes flatten made of that call (one). */
typedef struct { int edge; const pm_node_t *call; int taken; int deferred; } SpGraphSite;   /* deferred: sp_req_always_runs says no */

/* one file of the graph, read and parsed before any file
   is flattened, and its sites, in the order its tree has them. */
typedef struct {
  char *text;
  SpSrcFile src;
  int read;               /* text and src are set */
  int root;               /* its ProgramNode, once flattened */
  SpGraphSite *site; int nsite, sitecap;
  char **dirs; int ndirs; /* the directories the program had put on its load path when the file was first named */
  SpPreFileCtx pre;       /* pre-analysis constant-branch context for this file */
  int has_pre;
} SpGraphFile;

typedef struct {
  char lib_dir[1024];     /* where plain requires look first (sp_lib_dir) */
  char **file; int nfile, filecap;
  char **found; int foundcap;               /* [file] the path it was first found at, before
                                               symlinks are resolved (sp_graph_read) */
  unsigned char *flags; int flagcap;        /* [file] SP_FILE_* */
  SpLoadEdge *edge; int nedge, edgecap;     /* the tree's sites */
  SpLoadEdge *spl; int nspl, splcap;        /* the plain pass's resolutions */
  /* the files */
  SpGraphFile **gf; int gfcap;              /* [file], each where it was made: a
                                               parser is not moved once it has parsed */
  int *boot; int nboot, bootcap;            /* the edges of the libraries loaded at the
                                               program's start (sp_graph_boot) */
  int *entered;                             /* [file] where it runs in load order (--dump-load-graph) */
  char **iroots; int niroots;               /* the roots from -I, which the load path starts with */
  SpPreFileCtx pre_inherit;                 /* top-level shadowing propagated to subsequent files */
  int has_pre_inherit;
  int pre_tightened;                        /* set if a newly read file tightened pre_inherit */
} SpLoadGraph;

/* The file `path`'s id, added the first time it is named: one file per
   canonical path, which keeps the path it was first named by too. */
static int sp_load_file(SpLoadGraph *g, const char *path) {
  char *canon = sp_canonical_path(path);
  for (int i = 0; i < g->nfile; i++) if (strcmp(g->file[i], canon) == 0) { free(canon); return i; }
  g->file = sp_grow_strs(g->file, g->nfile, &g->filecap, 16);
  g->found = sp_grow_strs(g->found, g->nfile, &g->foundcap, 16);
  g->file[g->nfile] = canon;
  g->found[g->nfile] = strdup(path);
  if (!g->found[g->nfile]) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  return g->nfile++;
}

/* 1 the first time file `id`'s sites are asked for: a file read again
   (inlined under two conditions) records them once. */
enum { SP_FILE_WALKED = 1,    /* its sites are recorded */
       SP_FILE_BUILTIN = 2,    /* a builtins/ file: its nodes are builtin ones */
       SP_FILE_TIER_SHIFT = 2 };   /* bits 2..4: where `require` found it (SP_LIB_*) */
static unsigned char *sp_load_flags(SpLoadGraph *g, int id) {
  if (id >= g->flagcap) {
    int cap = g->flagcap ? g->flagcap : 16;
    while (cap <= id) cap *= 2;
    g->flags = realloc(g->flags, (size_t)cap);
    if (!g->flags) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    memset(g->flags + g->flagcap, 0, (size_t)(cap - g->flagcap));
    g->flagcap = cap;
  }
  return &g->flags[id];
}
static int sp_load_tier(SpLoadGraph *g, int id) { return (*sp_load_flags(g, id) >> SP_FILE_TIER_SHIFT) & 7; }
static void sp_load_set_tier(SpLoadGraph *g, int id, int tier) {
  if (id < 0) return;
  unsigned char *f = sp_load_flags(g, id);
  *f = (unsigned char)((*f & ~(7 << SP_FILE_TIER_SHIFT)) | (sp_tier_merge(sp_load_tier(g, id), tier) << SP_FILE_TIER_SHIFT));
}

static void sp_load_edge(SpLoadEdge **v, int *n, int *cap, int from, int line, int plain,
                         const char *name, int how, int to) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 16;
    *v = realloc(*v, sizeof **v * (size_t)*cap);
    if (!*v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  SpLoadEdge e = { from, line, plain, strdup(name), how, to, -1, 0, -1 };
  if (!e.name) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  (*v)[(*n)++] = e;
}

static int sp_lib_is_native(const char *name);
static int sp_require_tolerated(const char *name);
static const char *sp_package_feature(const char *lib_name);

/* A regular file that can be read: a directory named `foo.rb` is not one,
   and the lookup goes on past it, as it did when it read each candidate. */
static int sp_lib_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) && sp_path_exists(path);
}

/* Where `require "<lib_name>"` (".rb" taken off) finds its file, in the
   order the plain pass looks: lib/, lib/ one level up, the `-I` roots,
   builtins/, packages/ and packages/ one level up, the ffi and fiddle
   packages only where their object is built. The tier that answers
   (SP_LIB_*, node_table.h), with the file in `out`; SP_LIB_NONE when no
   file does. */
/* Pre-installed package `pkg`'s file for `feature`
   (packages/<pkg>/<feature>.rb), beside lib/ or one level up, into out: 0
   when there is none */
static int sp_package_file(const char *lib_dir, const char *pkg, const char *feature, char *out, size_t n) {
  int base_len = (int)strlen(lib_dir);
  if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
  snprintf(out, n, "%.*s/packages/%s/%s.rb", base_len, lib_dir, pkg, feature);
  if (sp_lib_file(out)) return 1;
  snprintf(out, n, "%.*s/../packages/%s/%s.rb", base_len, lib_dir, pkg, feature);
  return sp_lib_file(out);
}

static int sp_lib_lookup(const char *lib_dir, const char *lib_name, char *out, size_t n) {
  snprintf(out, n, "%s/%s.rb", lib_dir, lib_name);
  if (sp_lib_file(out)) return SP_LIB_LIB;
  /* the compiler binary may live one level below the repo root (e.g.
     build/spinel with the stdlib at ../lib) */
  int exe_len = (int)strlen(lib_dir) - 4;   /* strip the trailing "/lib" */
  if (exe_len < 0) exe_len = 0;
  snprintf(out, n, "%.*s/../lib/%s.rb", exe_len, lib_dir, lib_name);
  if (sp_lib_file(out)) return SP_LIB_LIB;
  /* `-I <dir>` feature roots: <root>/X.rb, else <root>/X/<last>.rb. They
     come before the pre-installed packages, so a project's package of the
     same name as a bundled one is the one a require reaches (#7207) */
  const char *last = strrchr(lib_name, '/');
  last = last ? last + 1 : lib_name;
  for (int ri = 0; ri < sp_feature_roots_n; ri++) {
    snprintf(out, n, "%s/%s.rb", sp_feature_roots[ri], lib_name);
    if (sp_lib_file(out)) return SP_LIB_ROOT;
    snprintf(out, n, "%s/%s/%s.rb", sp_feature_roots[ri], lib_name, last);
    if (sp_lib_file(out)) return SP_LIB_ROOT;
  }
  int base_len = (int)strlen(lib_dir);
  if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
  /* builtins/: the core methods written in Ruby, `require "builtins/<x>"`
     is <root>/builtins/<x>.rb */
  if (strncmp(lib_name, "builtins/", 9) == 0) {
    snprintf(out, n, "%.*s/%s.rb", base_len, lib_dir, lib_name);
    if (sp_lib_file(out)) return SP_LIB_BUILTIN;
    snprintf(out, n, "%.*s/../%s.rb", base_len, lib_dir, lib_name);
    if (sp_lib_file(out)) return SP_LIB_BUILTIN;
  }
  /* pre-installed packages (the carved-out stdlib), beside lib/: the package
     root is the require root, so `require "erb/util"` is
     packages/erb/erb/util.rb */
  const char *pkg = sp_package_feature(lib_name);
  if (!pkg) return SP_LIB_NONE;
  char first[256];
  const char *sl = strchr(pkg, '/');
  size_t fl = sl ? (size_t)(sl - pkg) : strlen(pkg);
  if (fl >= sizeof first) fl = sizeof first - 1;
  memcpy(first, pkg, fl);
  first[fl] = 0;
#if defined(_WIN32)
  /* a package Windows has no facility for (pty): none to find there */
  if (sp_w32_package_absent(first)) return SP_LIB_NONE;
#endif
  if (!sp_package_file(lib_dir, first, pkg, out, n)) return SP_LIB_NONE;
  /* the ffi package is glue over the system libffi and fiddle its native
     layer: without the built object the require stays the builtin DSL's */
  int ffi = strcmp(lib_name, "ffi") == 0, fiddle = strcmp(lib_name, "fiddle") == 0;
  if (ffi || fiddle) {
    char op[1200];
    snprintf(op, sizeof op, "%.*s%s", (int)(strlen(out) - strlen(ffi ? "ffi.rb" : "fiddle/fiddle.rb")), out,
             ffi ? "sp_ffi.o" : "ffi/sp_ffi.o");
    if (!sp_lib_file(op)) return SP_LIB_NONE;
  }
  return SP_LIB_PACKAGE;
}

/* What the compiler appends after a library file's own text, whichever
   load model reads the file: the one place both models ask, so they
   cannot differ on a file's text. Only the compiler's own libraries
   (`tier` is SP_LIB_BUILTIN or SP_LIB_PACKAGE, as the require that found
   the file says) have any. `content` is the file's text and is consumed;
   `path` is the file as found. First, what a native Windows build
   answers differently for it, lib/win32/overlay/<the same path>
   (sp_win32_driver.c). Then a package's share declarations
   (`native_share`), in <name>.share.rb beside the file. */
static char *sp_lib_text_appended(char *content, const char *path, int tier) {
  if (!content || tier < SP_LIB_BUILTIN) return content;
#if defined(_WIN32)
  content = sp_w32_overlay(content, path);
#endif
  if (strlen(path) > 3) {
    char sp[1100];
    snprintf(sp, sizeof sp, "%.*s.share.rb", (int)(strlen(path) - 3), path);
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
  return content;
}

/* Is `arg` of a kind that names no path (a number, a Symbol, nil, true,
   false, an Array or a Hash)? Kernel#require raises TypeError for it, so a
   call that passes one is a method of the program's own that has the
   name. */
static int sp_req_not_path(const pm_node_t *arg) {
  switch (PM_NODE_TYPE(arg)) {
    case PM_INTEGER_NODE: case PM_FLOAT_NODE: case PM_SYMBOL_NODE: case PM_NIL_NODE: case PM_TRUE_NODE:
    case PM_FALSE_NODE: case PM_ARRAY_NODE: case PM_HASH_NODE: return 1;
    default: return 0;
  }
}

/* Is `n` a require the load model loads: `require` or `require_relative`,
   receiverless or on Kernel, with one argument that can name a path and no
   block? Or an autoload with two arguments and no block, receiverless or on
   self or a constant, the receivers that can be a module: on any other
   value it is a method of the program's own or a NoMethodError, which
   loads nothing. */
static int sp_load_call(const pm_call_node_t *n) {
  if (sp_pm_name_is(g_parser, n->name, "autoload")) {
    pm_node_type_t rt = n->receiver ? PM_NODE_TYPE(n->receiver) : PM_SELF_NODE;
    return !n->block && n->arguments && n->arguments->arguments.size == 2 &&
           PM_NODE_TYPE(n->arguments->arguments.nodes[1]) != PM_SPLAT_NODE &&
           (rt == PM_SELF_NODE || rt == PM_CONSTANT_READ_NODE || rt == PM_CONSTANT_PATH_NODE);
  }
  if (!sp_pm_name_is(g_parser, n->name, "require") && !sp_pm_name_is(g_parser, n->name, "require_relative"))
    return 0;
  if (n->receiver && (!sp_call_receiver_is_kernel(g_parser, n->receiver) ||
                      !n->call_operator_loc.start || *n->call_operator_loc.start != '.')) return 0;
  return !n->block && n->arguments && n->arguments->arguments.size == 1 &&
         PM_NODE_TYPE(n->arguments->arguments.nodes[0]) != PM_SPLAT_NODE &&
         !sp_req_not_path(n->arguments->arguments.nodes[0]);
}

/* Sites by file, then line (the plain ones were noted first). */
static int sp_load_edge_cmp(const void *x, const void *y) {
  const SpLoadEdge *a = x, *b = y;
  return a->from != b->from ? a->from - b->from : a->line - b->line;
}

static void sp_load_graph_dump(const SpLoadGraph *g) {
  static const char *const how[] = { "", "native", "core", "unresolved", "run-time", "set" };
  /* the sites in an insertion-sorted copy, which keeps two sites of one
     line in the order they were noted (the edges' own order is what the
     load order's indices name) */
  SpLoadEdge *sorted = malloc(sizeof *sorted * (size_t)(g->nedge + 1));
  if (!sorted) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (g->nedge) memcpy(sorted, g->edge, sizeof *sorted * (size_t)g->nedge);
  for (int i = 1; i < g->nedge; i++)
    for (int j = i; j > 0 && sp_load_edge_cmp(&sorted[j - 1], &sorted[j]) > 0; j--) {
      SpLoadEdge t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
    }
  printf("load graph: %d files, %d sites\n", g->nfile, g->nedge);
  for (int i = 0; i < g->nfile; i++) printf("file %d %s\n", i, g->file[i]);
  for (int k = 0; k < 2; k++) {
    const SpLoadEdge *v = k ? g->spl : sorted;
    int n = k ? g->nspl : g->nedge;
    for (int i = 0; i < n; i++) {
      if (k) printf("splice require \"%s\" -> ", v[i].name);
      else printf("site %d:%d %s \"%s\" -> ", v[i].from, v[i].line, v[i].plain ? "require" : "require_relative",
                  v[i].name);
      if (v[i].how == SP_LOAD_FILE) printf("%d\n", v[i].to);
      else printf("%s\n", how[v[i].how]);
    }
  }
  free(sorted);
}

static void sp_load_graph_free(SpLoadGraph *g) {
  for (int i = 0; i < g->gfcap; i++) {
    SpGraphFile *r = g->gf[i];
    if (!r) continue;
    if (r->read) { sp_src_file_free(&r->src); free(r->text); }
    if (r->has_pre) sp_pre_ctx_free(&r->pre);
    free(r->site);
    for (int k = 0; k < r->ndirs; k++) free(r->dirs[k]);
    free(r->dirs);
    free(r);
  }
  if (g->has_pre_inherit) sp_pre_ctx_free(&g->pre_inherit);
  free(g->gf); free(g->boot); free(g->entered);
  for (int i = 0; i < g->niroots; i++) free(g->iroots[i]);
  free(g->iroots);
  for (int i = 0; i < g->nfile; i++) { free(g->file[i]); free(g->found[i]); }
  for (int i = 0; i < g->nedge; i++) {
    free(g->edge[i].name);
    for (int k = 0; k < g->edge[i].nauto; k++) { free(g->edge[i].autoload[k]); free(g->edge[i].auto_path[k]); }
    free(g->edge[i].autoload); free(g->edge[i].auto_path); free(g->edge[i].auto_file);
    free(g->edge[i].cand);
    free(g->edge[i].pat_pre); free(g->edge[i].pat_suf);
  }
  for (int i = 0; i < g->nspl; i++) free(g->spl[i].name);
  free(g->file); free(g->found); free(g->flags); free(g->edge); free(g->spl);
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
   no-ops (the feature is already loaded), so the strict-require tolerates them
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
     Refused under --strict-require, they broke every spin build using the
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

/* ---- builtins/: the other containers (Integer, Float, Comparable, Monitor) ----
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
  { "builtins/monitor.rb", "builtins/monitor", NULL, 0, 0 },
};
#define SP_BUILTIN_EXTRA_N ((int)(sizeof(sp_builtin_extras) / sizeof(sp_builtin_extras[0])))

/* Accessors for analyze_desugar.c (desugar_builtin_scalar_defs/_calls):
   sp_builtin_extras is static to this file, and its `names` arrays are
   only populated lazily (the file is read on first use, from
   sp_splice_builtin_extra above), which by the time analysis runs has
   already happened for every extra a program's source could have
   mentioned. Container indices match SP_BUILTIN_EXTRA_N's table order:
   0 = Integer, 1 = Float, 2 = Comparable, 3 = Monitor. */
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
   than nothing at all. Graph mode runs the pass once per file, and the
   warning is still said once for the program. */
static int sp_inline_rbs_run(pm_parser_t *parser, pm_node_t *root, const char *source, size_t len,
                             const char *path, int fid) {
  static int said = 0;
  (void)root; (void)len; (void)fid;
  if (!sp_inline_rbs_enabled || said) return 0;
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
      const char *file = path;
      int line = bl;
      if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n && sp_line_orig[bl] > 0) { file = sp_file_table[sp_line_file[bl]]; line = sp_line_orig[bl]; }
      fprintf(stderr, "spinel: %s:%d: warning: inline RBS comments are not applied: this spinel was "
                      "built without the rbs parser (run `make deps` and rebuild, or pass --no-inline-rbs)\n", file, line);
      said = 1;
      return 0;
    }
  }
  return 0;
}
static void sp_inline_rbs_emit(const pm_node_t *node, int id) { (void)node; (void)id; }
static int sp_inline_rbs_any(pm_parser_t *parser) { (void)parser; return 0; }
static void sp_inline_rbs_declare(pm_parser_t *parser, pm_node_t *root, const char *source, size_t len) {
  (void)parser; (void)root; (void)source; (void)len;
}
static void sp_inline_rbs_done(void) {}
static void sp_inline_rbs_free(void) {}
#endif

/* Parse `source_file` into a new node table. `argv0` is the invoking
   program path (used to locate the stdlib for plain `require`s). Returns
   the table, or NULL on a read or parse error. This is the library copy
   (the in-process lib API; no standalone CLI main). */
/* What a require of file `from` of graph `g` loads -- `method` of `path` (NULL
   for an expression computed when it runs), on `line` -- recorded as an edge
   of the graph, whose index it answers: a file of the graph (added the first
   time), the runtime's native stand-in, a core no-op, nothing (warned at
   compile time, LoadError when it runs), or a path computed when it runs. */
/* What a require_relative of `path` in file `from` names: the path from the
   file's own directory, as CRuby expands it (before any ".rb"). */
static void sp_load_rel_path(const SpLoadGraph *g, int from, const char *path, char *out, size_t cap) {
  char *dir = strdup(g->file[from]), *sl = dir ? strrchr(dir, '/') : NULL;
  if (!dir) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (sl) *sl = 0;
  snprintf(out, cap, path[0] == '/' ? "%s%s" : "%s/%s", path[0] == '/' ? "" : dir, path);
  free(dir);
  sp_normalize_dots(out);
}

static void sp_graph_inherit(SpLoadGraph *g, int f);

/* The program's file a require of `path` (relative to the file when `rel`)
   from file `from` names, in found[cap]: 1 when there is one, its tier in
   *tier (0 for a relative one). */
static int sp_load_find(SpLoadGraph *g, int from, int rel, const char *path, char *found, size_t cap, int *tier) {
  *tier = 0;
  if (rel) {
    sp_load_rel_path(g, from, path, found, cap);
    size_t fl = strlen(found);
    if (fl < cap - 4 && (fl < 3 || strcmp(found + fl - 3, ".rb") != 0)) strcat(found, ".rb");
    return sp_lib_file(found);
  }
  char lib_name[256];
  snprintf(lib_name, sizeof lib_name, "%s", path);
  size_t ln = strlen(lib_name);
  if (ln > 3 && strcmp(lib_name + ln - 3, ".rb") == 0) lib_name[ln - 3] = '\0';
  return (*tier = sp_lib_lookup(g->lib_dir, lib_name, found, cap)) != 0;
}

/* What a require of `path` (relative to the file when `rel`) from file
   `from` names: SP_LOAD_*, the target file in `to` for SP_LOAD_FILE (-1
   otherwise). The features it names and enables are noted. */
static int sp_load_target(SpLoadGraph *g, int from, int rel, const char *path, int *to) {
  int tier;
  char found[1024];
  int how = sp_load_find(g, from, rel, path, found, sizeof found, &tier) ? SP_LOAD_FILE : SP_LOAD_NONE;
  if (rel) {
    char rn[512];
    snprintf(rn, sizeof rn, "%s", path);
    size_t rl = strlen(rn);
    if (rl > 3 && strcmp(rn + rl - 3, ".rb") == 0) rn[rl - 3] = '\0';
    sp_note_required_feature(rn[0] == '.' && rn[1] == '/' ? rn + 2 : rn);
  }
  else {
    char lib_name[256];
    snprintf(lib_name, sizeof lib_name, "%s", path);
    size_t ln = strlen(lib_name);
    if (ln > 3 && strcmp(lib_name + ln - 3, ".rb") == 0) lib_name[ln - 3] = '\0';
    if (how == SP_LOAD_NONE && sp_lib_is_native(lib_name)) how = SP_LOAD_NATIVE;
    else if (how == SP_LOAD_NONE && sp_require_tolerated(lib_name)) how = SP_LOAD_CORE;
    /* the features a require enables (strict require) and the ones it
       names */
    if (how == SP_LOAD_FILE || how == SP_LOAD_NATIVE) sp_feature_mark(lib_name);
    if (how != SP_LOAD_NONE) sp_note_required_feature(lib_name);
  }
  int known = g->nfile;
  *to = how == SP_LOAD_FILE ? sp_load_file(g, found) : -1;
  if (*to >= known) sp_graph_inherit(g, *to);
  if (tier == SP_LIB_BUILTIN) *sp_load_flags(g, *to) |= SP_FILE_BUILTIN;
  sp_load_set_tier(g, *to, tier);
  return how;
}

/* A require written in a file read for g_require_unread, resolved: a file
   of the graph, read before or now, or what the runtime provides (or a
   closed set of them) */
static void sp_load_resolved(SpLoadGraph *g, int from) {
  if (!(*sp_load_flags(g, from) & SP_FILE_BUILTIN)) sp_req_resolved++;
}

/* `force_rel`: the path is the file's directory's (a path built on `__dir__`),
   whichever the call. */
static int sp_load_resolve(SpLoadGraph *g, int from, const char *method, const char *path, int line, int force_rel) {
  int rel = force_rel || (method && strcmp(method, "require_relative") == 0), to = -1;
  int how = sp_load_target(g, from, rel, path, &to);
  if (how == SP_LOAD_FILE || how == SP_LOAD_NATIVE || how == SP_LOAD_CORE) sp_load_resolved(g, from);
  sp_load_edge(&g->edge, &g->nedge, &g->edgecap, from, line, !rel, path, how, to);
  return g->nedge - 1;
}

/* ---- What the program names, read from the trees (graph mode) ----
 *
 * The libraries CRuby has loaded before a program runs -- builtins/, Set,
 * IO::Buffer (answer 6 of #9898) -- and the add-ons a require makes
 * available (CRuby's time.rb for `require "time"`) load when the program's
 * trees name something they define: one rule, a reachability test on names,
 * asked to a fixpoint because a loaded library's own names count. It is the
 * coarse filter: what a loaded library's methods cost is the analysis's
 * per-method reachability, the fine one. The names, tagged:
 *   m:  a method name: a call, a Symbol (`:name`, a `%i[]` word, not a hash
 *       key's `name:`), a String or Symbol a method-name reflection is
 *       given (`send("tally")`), and the def a `super` is in
 *   c:  a constant: each one read, a path whole (`IO::Buffer`) and a
 *       top-level `::X`; a `class X` nested in another names no top-level X
 *   l:  a compiler-private helper (`__sp_popen`), which only a lowering
 *       reaches: the shape rows (LowerRow) a call matches
 *   s:  a String literal that reads as a name, and dyn: the program has a
 *       method-name reflection or a const_get of a computed name. Where it
 *       does, the literals count as names, the bound the analysis puts on
 *       such a call (cf_collect_dyn): a name built at run time is not seen.
 * A String an `eval` family call is given is read as the code it is, an
 * interpolation in it as a name. */
typedef struct { char **v; int n, cap, sorted; } SpNames;

static void sp_names_add(SpNames *s, const char *tag, const char *b, size_t blen) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->v = realloc(s->v, sizeof *s->v * (size_t)s->cap);
    if (!s->v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  size_t tl = strlen(tag);
  char *k = malloc(tl + blen + 1);
  if (!k) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  memcpy(k, tag, tl); memcpy(k + tl, b, blen); k[tl + blen] = '\0';
  s->v[s->n++] = k;
  s->sorted = 0;
}

static int sp_names_cmp(const void *x, const void *y) {
  return strcmp(*(char *const *)x, *(char *const *)y);
}

/* (sorted once per batch of adds, a name kept once) */
static int sp_names_has(SpNames *s, const char *key) {
  if (!s->sorted) {
    qsort(s->v, (size_t)s->n, sizeof *s->v, sp_names_cmp);
    int k = 0;
    for (int i = 0; i < s->n; i++) {
      if (k && strcmp(s->v[k - 1], s->v[i]) == 0) { free(s->v[i]); continue; }
      s->v[k++] = s->v[i];
    }
    s->n = k;
    s->sorted = 1;
  }
  return bsearch(&key, s->v, (size_t)s->n, sizeof *s->v, sp_names_cmp) != NULL;
}

static void sp_names_free(SpNames *s) {
  for (int i = 0; i < s->n; i++) free(s->v[i]);
  free(s->v);
  memset(s, 0, sizeof *s);
}

/* `name` (len bytes) under the tag a lowering's target takes: a constant's
   for a capitalized one (Set), a helper's for a `__` one, else a method's */
static const char *sp_names_target_tag(const char *name) {
  if (name[0] >= 'A' && name[0] <= 'Z') return "c:";
  return name[0] == '_' && name[1] == '_' ? "l:" : "m:";
}

typedef struct {
  SpNames *s;
  const pm_parser_t *p;
  int nested;                 /* inside a class or module body */
  pm_constant_id_t def;       /* the def the walk is in, 0 outside one */
  const pm_node_t *defined;   /* the operand of the defined? the walk is at: it does not run */
  const SpPreFileCtx *pre;    /* skip provably dead constant-guard branches */
  int in_bo;                  /* inside a BasicObject subclass */
} SpNamesWalk;

static void sp_names_const(SpNamesWalk *w, const char *tag, pm_constant_id_t id) {
  const pm_constant_t *c = pm_constant_pool_id_to_constant(&w->p->constant_pool, id);
  sp_names_add(w->s, tag, (const char *)c->start, c->length);
}

/* `A::B::C` written into buf (cap bytes) from its constants, 0 when a part
   is not a constant (`x::C`) or it does not fit */
static int sp_names_path(const pm_parser_t *p, const pm_node_t *n, char *buf, size_t cap) {
  if (!n || cap < 2) return 0;
  pm_constant_id_t id;
  size_t at = 0;
  if (PM_NODE_TYPE(n) == PM_CONSTANT_READ_NODE) id = ((const pm_constant_read_node_t *)n)->name;
  else if (PM_NODE_TYPE(n) == PM_CONSTANT_PATH_NODE) {
    const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)n;
    id = cp->name;
    if (cp->parent) {
      if (!sp_names_path(p, cp->parent, buf, cap)) return 0;
      at = strlen(buf);
      if (at + 2 >= cap) return 0;
      buf[at++] = ':'; buf[at++] = ':';
    }
  }
  else return 0;
  const pm_constant_t *c = pm_constant_pool_id_to_constant(&p->constant_pool, id);
  if (at + c->length + 1 > cap) return 0;
  memcpy(buf + at, c->start, c->length);
  buf[at + c->length] = '\0';
  return 1;
}

/* Does `n` hold a `break` that leaves the block it sits in, rather than a
   loop or a block nested inside it? (block_body_breaks, of a parsed body) */
static bool sp_pm_breaks_visit(const pm_node_t *n, void *data) {
  switch (PM_NODE_TYPE(n)) {
    case PM_BREAK_NODE: *(int *)data = 1; return false;
    case PM_WHILE_NODE: case PM_UNTIL_NODE: case PM_FOR_NODE: case PM_BLOCK_NODE: case PM_LAMBDA_NODE:
    case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE: return false;
    default: return !*(int *)data;
  }
}
static int sp_pm_block_breaks(const pm_node_t *body) {
  int hit = 0;
  if (body) pm_visit_node(body, sp_pm_breaks_visit, &hit);
  return hit;
}

/* Whether call `c` passes lo..hi arguments of the kinds `flags` allows and
   has the block and the receiver they ask for (lower_row_nt's test, of a
   parsed call) */
static int sp_pm_args_fit(const pm_call_node_t *c, int lo, int hi, unsigned flags, int drop) {
  const pm_node_list_t *args = c->arguments ? &c->arguments->arguments : NULL;
  int an = (args ? (int)args->size : 0) - drop;
  if (an < lo || (hi >= 0 && an > hi)) return 0;
  const pm_node_t *blk = c->block;
  if ((flags & LR_NO_BLOCK) && blk) return 0;
  if ((flags & LR_BLOCK) && !blk) return 0;
  /* a `&:name` flatten makes a block of is a literal block, one that never breaks */
  const char *sym;
  int made = blk && sp_sym_proc_name(blk, &sym) > 0;
  if ((flags & LR_BLOCK_LITERAL) && !made && (!blk || PM_NODE_TYPE(blk) != PM_BLOCK_NODE)) return 0;
  if ((flags & LR_BREAKS) && (!blk || PM_NODE_TYPE(blk) != PM_BLOCK_NODE)) return 0;
  if ((flags & LR_HAS_RECV) && !c->receiver) return 0;
  if ((flags & LR_NO_ARGS) && an > 0) return 0;
  int kwh = 0;
  for (int i = 0; i < an; i++) {
    pm_node_type_t k = PM_NODE_TYPE(args->nodes[i]);
    if (k == PM_KEYWORD_HASH_NODE) kwh = 1;
    if ((flags & (LR_NO_SPLAT | LR_PLAIN_ARGS)) && k == PM_SPLAT_NODE) return 0;
    if ((flags & LR_PLAIN_ARGS) && (k == PM_KEYWORD_HASH_NODE || k == PM_BLOCK_ARGUMENT_NODE)) return 0;
  }
  if ((flags & LR_KWHASH) && !kwh) return 0;
  if ((flags & LR_BREAKS) && !sp_pm_block_breaks(((const pm_block_node_t *)blk)->body)) return 0;
  return 1;
}

/* Is `n` the constant read `name`? */
static int sp_pm_const_is(const pm_parser_t *p, const pm_node_t *n, const char *name) {
  return n && PM_NODE_TYPE(n) == PM_CONSTANT_READ_NODE &&
         sp_pm_name_is(p, ((const pm_constant_read_node_t *)n)->name, name);
}

/* Does call `c` (named `name`, parsed by `p`) have the shape of lowering row
   `r`? lower_row_nt's question, of a parsed call: the graph decides its
   libraries before flattening. SPINEL_LOWER_ROWS_CHECK holds the two to
   one answer (sp_lower_rows_note, lower_rows_check). */
static int sp_pm_row_fits(const pm_parser_t *p, const pm_call_node_t *c, const char *name, const LowerRow *r, int drop) {
  if (strcmp(name, r->name) != 0) return 0;
  const pm_node_t *recv = c->receiver;
  if (r->recv == LR_RECV_NONE && recv) return 0;
  if (r->recv == LR_RECV_SOME && !recv) return 0;
  if (r->recv == LR_RECV_CONST && !sp_pm_const_is(p, recv, r->recv_name)) return 0;
  if (r->recv == LR_RECV_CALL) {
    if (!recv || PM_NODE_TYPE(recv) != PM_CALL_NODE) return 0;
    const pm_call_node_t *rc = (const pm_call_node_t *)recv;
    if (!sp_pm_name_is(p, rc->name, r->recv_name)) return 0;
    if (r->recv_const && !sp_pm_const_is(p, rc->receiver, r->recv_const)) return 0;
    if (!sp_pm_args_fit(rc, r->rargc_min, r->rargc_max, r->rflags, 0)) return 0;
  }
  if (r->recv_calls && recv && PM_NODE_TYPE(recv) == PM_CALL_NODE && !((const pm_call_node_t *)recv)->block) {
    int k = 0;
    while (r->recv_calls[k] && !sp_pm_name_is(p, ((const pm_call_node_t *)recv)->name, r->recv_calls[k])) k++;
    if (!r->recv_calls[k]) return 0;
  }
  return sp_pm_args_fit(c, r->argc_min, r->argc_max, r->flags, drop);
}

/* Does call `c` end in `**` operands that may all pass nothing (an empty
   operand)? Then the analysis splits it into the call with them and the call
   without (split_kwsplat_calls in analyze.c, whose rule this one repeats:
   not the operand of `defined?` (`defined`, the node the walk is at, or
   NULL where it is not known: that call is never split, so the shape this
   notes for it is never looked for), no splat beside the `**`, no pair-only
   hash literal among the operands, which is never empty); the one without
   takes the rows of its own shape. */
static int sp_pm_kwsplat_may_drop(const pm_call_node_t *c, const pm_node_t *defined) {
  const pm_node_list_t *args = c->arguments ? &c->arguments->arguments : NULL;
  if (!args || !args->size || PM_NODE_TYPE(args->nodes[args->size - 1]) != PM_KEYWORD_HASH_NODE ||
      (const pm_node_t *)c == defined)
    return 0;
  const pm_node_list_t *el = &((const pm_keyword_hash_node_t *)args->nodes[args->size - 1])->elements;
  int drop = 1;
  for (size_t k = 0; k < args->size - 1; k++) {
    pm_node_type_t t = PM_NODE_TYPE(args->nodes[k]);
    if (t == PM_SPLAT_NODE || t == PM_FORWARDING_ARGUMENTS_NODE) drop = 0;
  }
  for (size_t k = 0; k < el->size; k++) {
    if (PM_NODE_TYPE(el->nodes[k]) != PM_ASSOC_SPLAT_NODE) { drop = 0; continue; }
    const pm_node_t *v = ((const pm_assoc_splat_node_t *)el->nodes[k])->value;
    if (!v || PM_NODE_TYPE(v) != PM_HASH_NODE) continue;
    const pm_node_list_t *he = &((const pm_hash_node_t *)v)->elements;
    int has_splat = 0;
    for (size_t q = 0; q < he->size; q++) if (PM_NODE_TYPE(he->nodes[q]) == PM_ASSOC_SPLAT_NODE) has_splat = 1;
    if (he->size && !has_splat) drop = 0;
  }
  return drop;
}

/* The lowerings call `c` has the shape of (builtin_names.c's rows): each
   names the method or constant it lowers to */
static void sp_names_lowerings(SpNamesWalk *w, const pm_call_node_t *c, const char *name) {
  const LowerRow *r;
  int drop = sp_pm_kwsplat_may_drop(c, w->defined);
  for (int i = 0; (r = lower_row_at(i)) != NULL; i++)
    if (sp_pm_row_fits(w->p, c, name, r, 0) || (drop && sp_pm_row_fits(w->p, c, name, r, 1)))
      sp_names_add(w->s, sp_names_target_tag(r->target), r->target, strlen(r->target));
}

/* SPINEL_LOWER_ROWS_CHECK: each call flattened in graph mode notes the
   lowering rows its parse has the shape of (sp_pm_row_fits), a mask of
   row indices (`lr_rows`); the analysis's lower_rows_check compares it
   with the rows the node it became matches (lower_row_nt). A call that may
   be split on its `**` operands (sp_pm_kwsplat_may_drop) notes the rows of
   the call without them too (`lr_rows_drop`), which split_kwsplat_calls
   hands to the call it builds, so both shapes are checked. Only the first
   row of a family is compared: the names the graph loads for a call are a
   superset of the rows' targets by construction (sp_names_add), and extra
   names are not this check's business. */
static int g_lower_rows_check = -1;
static void sp_lower_rows_note(const pm_node_t *node, int id) {
  if (g_lower_rows_check < 0) g_lower_rows_check = getenv("SPINEL_LOWER_ROWS_CHECK") != NULL;
  if (!g_lower_rows_check || !node || PM_NODE_TYPE(node) != PM_CALL_NODE || id < 0) return;
  const pm_call_node_t *c = (const pm_call_node_t *)node;
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&g_parser->constant_pool, c->name);
  char name[64];
  snprintf(name, sizeof name, "%.*s", (int)nm->length, (const char *)nm->start);
  long long mask = 0, dmask = 0;
  const LowerRow *r;
  int drop = sp_pm_kwsplat_may_drop(c, NULL);
  for (int i = 0; i < LOWER_ROWS_MASK_BITS && (r = lower_row_at(i)) != NULL; i++) {
    if (sp_pm_row_fits(g_parser, c, name, r, 0)) mask |= 1LL << i;
    if (drop && sp_pm_row_fits(g_parser, c, name, r, 1)) dmask |= 1LL << i;
  }
  emit_int(id, "lr_rows", mask);
  if (drop) emit_int(id, "lr_rows_drop", dmask);
}

/* Method name `b` given as a value (a Symbol, a String a reflection is
   given): a row that asks only the name and a receiver (an Enumerable
   alias, to_set) is reached by it as by a call, whatever arguments the
   call is given (a superset only loads more) */
static void sp_names_method(SpNames *s, const uint8_t *b, size_t n) {
  sp_names_add(s, "m:", (const char *)b, n);
  const LowerRow *r;
  for (int i = 0; (r = lower_row_at(i)) != NULL; i++)
    if ((r->recv == LR_RECV_ANY || r->recv == LR_RECV_SOME) && !r->flags && r->argc_min == 0 &&
        strlen(r->name) == n && memcmp(r->name, b, n) == 0)
      sp_names_add(s, sp_names_target_tag(r->target), r->target, strlen(r->target));
}

static void sp_names_of(SpNames *s, const pm_parser_t *p, const pm_node_t *root, const SpPreFileCtx *pre);

static void sp_names_call(SpNamesWalk *w, const pm_call_node_t *c) {
  const pm_constant_t *nm = pm_constant_pool_id_to_constant(&w->p->constant_pool, c->name);
  sp_names_add(w->s, "m:", (const char *)nm->start, nm->length);
  /* (every name a row or a family below asks is shorter) */
  if (nm->length >= 32) return;
  char name[32];
  memcpy(name, nm->start, nm->length);
  name[nm->length] = '\0';
  sp_names_lowerings(w, c, name);
  const pm_node_list_t *args = c->arguments ? &c->arguments->arguments : NULL;
  const pm_node_t *a0 = args && args->size ? args->nodes[0] : NULL;
  int t = a0 ? (int)PM_NODE_TYPE(a0) : 0;
  /* a name given by a value: a String names as a Symbol does, a computed
     one leaves the program's literals to bound it */
  int reflect = is_method_name_reflection(name), cget = is_const_query_name(name);
  if (reflect || cget) {
    if (t == PM_STRING_NODE) {
      const pm_string_t *str = &((const pm_string_node_t *)a0)->unescaped;
      if (reflect) sp_names_method(w->s, pm_string_source(str), pm_string_length(str));
      else sp_names_add(w->s, "c:", (const char *)pm_string_source(str), pm_string_length(str));
    }
    else if (t == PM_SYMBOL_NODE) {
      /* respond_to?(:to_set), send(:to_set): the method the Symbol names */
      const pm_string_t *str = &((const pm_symbol_node_t *)a0)->unescaped;
      if (reflect) sp_names_method(w->s, pm_string_source(str), pm_string_length(str));
      else sp_names_add(w->s, "c:", (const char *)pm_string_source(str), pm_string_length(str));
    }
    else if (a0) sp_names_add(w->s, "dyn", "", 0);   /* (`req.method` names none) */
  }
  /* a Symbol made of a String names what the String says */
  if (is_symbol_conversion(name)) sp_names_add(w->s, "dyn", "", 0);
  if (!is_code_eval_name(name) || !a0) return;
  if (t != PM_STRING_NODE && t != PM_INTERPOLATED_STRING_NODE) {
    sp_names_add(w->s, "dyn", "", 0);   /* code built at run time */
    return;
  }
  /* the code, an interpolation read as a name (`def watch_#{kind}` is a def) */
  const pm_node_list_t *parts = t == PM_STRING_NODE ? NULL : &((const pm_interpolated_string_node_t *)a0)->parts;
  size_t len = 0, np = parts ? parts->size : 1;
  for (size_t i = 0; i < np; i++) {
    const pm_node_t *pt = parts ? parts->nodes[i] : a0;
    len += PM_NODE_TYPE(pt) == PM_STRING_NODE ? pm_string_length(&((const pm_string_node_t *)pt)->unescaped) : 1;
  }
  char *code = malloc(len + 1);
  if (!code) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  len = 0;
  for (size_t i = 0; i < np; i++) {
    const pm_node_t *pt = parts ? parts->nodes[i] : a0;
    if (PM_NODE_TYPE(pt) != PM_STRING_NODE) { code[len++] = 'x'; continue; }
    const pm_string_t *str = &((const pm_string_node_t *)pt)->unescaped;
    memcpy(code + len, pm_string_source(str), pm_string_length(str));
    len += pm_string_length(str);
  }
  code[len] = '\0';
  pm_parser_t ep;
  sp_pm_parser_init(&ep, code, len, NULL);
  pm_node_t *er = pm_parse(&ep);
  sp_names_of(w->s, &ep, er, NULL);
  pm_node_destroy(&ep, er);
  pm_parser_free(&ep);
  free(code);
}

/* Does the String read as a method or constant name? */
static int sp_names_word(const uint8_t *b, size_t n) {
  if (!n || n > 64 || !(isalpha(b[0]) || b[0] == '_')) return 0;
  for (size_t i = 1; i < n; i++)
    if (!(isalnum(b[i]) || b[i] == '_' || (i == n - 1 && (b[i] == '?' || b[i] == '!' || b[i] == '=')))) return 0;
  return 1;
}

static bool sp_names_visit(const pm_node_t *n, void *data) {
  SpNamesWalk *w = data;
  switch (PM_NODE_TYPE(n)) {
    case PM_IN_NODE: {
      const pm_in_node_t *in = (const pm_in_node_t *)n;
      const SpPreFileCtx *saved = w->pre;
      w->pre = NULL;
      if (in->pattern) pm_visit_node(in->pattern, sp_names_visit, w);
      w->pre = saved;
      if (in->statements) pm_visit_node((const pm_node_t *)in->statements, sp_names_visit, w);
      return false;
    }
    case PM_IF_NODE: case PM_UNLESS_NODE: {
      if (w->pre) {
        int is_unless = PM_NODE_TYPE(n) == PM_UNLESS_NODE;
        const pm_node_t *pred = is_unless ? ((const pm_unless_node_t *)n)->predicate : ((const pm_if_node_t *)n)->predicate;
        const pm_node_t *body = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)n)->statements
                                          : (const pm_node_t *)((const pm_if_node_t *)n)->statements;
        const pm_node_t *other = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)n)->else_clause
                                           : ((const pm_if_node_t *)n)->subsequent;
        int tr = sp_pre_pm_truth(w->p, pred, w->pre, !w->in_bo, 0);
        if (tr >= 0) {
          const pm_node_t *live = (is_unless ? !tr : tr) ? body : other;
          if (live) pm_visit_node(live, sp_names_visit, w);
          return false;
        }
      }
      break;
    }
    case PM_CALL_NODE: sp_names_call(w, (const pm_call_node_t *)n); break;
    case PM_DEFINED_NODE: w->defined = ((const pm_defined_node_t *)n)->value; break;
    case PM_SYMBOL_NODE: {
      /* `:name` or a `%i[]` word (a hash key's `name:` names no method) */
      const pm_symbol_node_t *sy = (const pm_symbol_node_t *)n;
      if (sy->opening_loc.start || !sy->closing_loc.start)
        sp_names_method(w->s, pm_string_source(&sy->unescaped), pm_string_length(&sy->unescaped));
      break;
    }
    case PM_STRING_NODE: {
      const pm_string_t *str = &((const pm_string_node_t *)n)->unescaped;
      if (sp_names_word(pm_string_source(str), pm_string_length(str)))
        sp_names_add(w->s, "s:", (const char *)pm_string_source(str), pm_string_length(str));
      break;
    }
    case PM_ALIAS_METHOD_NODE: {
      const pm_node_t *old = ((const pm_alias_method_node_t *)n)->old_name;
      if (PM_NODE_TYPE(old) == PM_SYMBOL_NODE) {
        const pm_string_t *str = &((const pm_symbol_node_t *)old)->unescaped;
        sp_names_add(w->s, "m:", (const char *)pm_string_source(str), pm_string_length(str));
      }
      break;
    }
    case PM_SUPER_NODE: case PM_FORWARDING_SUPER_NODE:
      if (w->def) sp_names_const(w, "m:", w->def);
      break;
    case PM_CONSTANT_READ_NODE: sp_names_const(w, "c:", ((const pm_constant_read_node_t *)n)->name); break;
    case PM_CONSTANT_WRITE_NODE: sp_names_const(w, "c:", ((const pm_constant_write_node_t *)n)->name); break;
    case PM_CONSTANT_PATH_NODE: {
      /* `A::B` names A (its own read) and the path, not a top-level B */
      const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *)n;
      char path[256];
      if (sp_names_path(w->p, n, path, sizeof path)) sp_names_add(w->s, "c:", path, strlen(path));
      if (!cp->parent) sp_names_const(w, "c:", cp->name);
      break;
    }
    case PM_CLASS_NODE: case PM_MODULE_NODE: {
      /* a `module X` nested in another opens Outer::X, no top-level X */
      int is_class = PM_NODE_TYPE(n) == PM_CLASS_NODE;
      const pm_node_t *cpath = is_class ? ((const pm_class_node_t *)n)->constant_path
                                        : ((const pm_module_node_t *)n)->constant_path;
      if (!(w->nested && PM_NODE_TYPE(cpath) == PM_CONSTANT_READ_NODE)) pm_visit_node(cpath, sp_names_visit, w);
      if (is_class && ((const pm_class_node_t *)n)->superclass)
        pm_visit_node(((const pm_class_node_t *)n)->superclass, sp_names_visit, w);
      const pm_node_t *body = is_class ? ((const pm_class_node_t *)n)->body : ((const pm_module_node_t *)n)->body;
      int was = w->nested;
      int is_bo = is_class && sp_pre_pm_is_basic_object(w->p, ((const pm_class_node_t *)n)->superclass);
      w->nested = 1;
      w->in_bo += is_bo;
      if (body) pm_visit_node(body, sp_names_visit, w);
      w->in_bo -= is_bo;
      w->nested = was;
      return false;
    }
    case PM_DEF_NODE: {
      pm_constant_id_t was = w->def;
      w->def = ((const pm_def_node_t *)n)->name;
      pm_visit_child_nodes(n, sp_names_visit, w);
      w->def = was;
      return false;
    }
    default: break;
  }
  return true;
}

static void sp_names_of(SpNames *s, const pm_parser_t *p, const pm_node_t *root, const SpPreFileCtx *pre) {
  SpNamesWalk w = { s, p, 0, 0, NULL, pre, 0 };
  if (root) pm_visit_node(root, sp_names_visit, &w);
}

/* ---- What a library defines that a program can reach ----
 * A library's entry names, read from its own tree: the methods it adds to
 * a class or module Spinel has without it (the top level's Object, a
 * native class: is_native_class_name), and the constants it creates. A
 * class it creates is reached through its constant; its methods are no
 * entry names. A `__` method is the compiler's own: an l: name, reached
 * only by a lowering. An add-on's definitions of a Time format the runtime
 * answers itself (is_time_native_format) are not what the program reaches. */
typedef struct {
  SpNames *s;
  const pm_parser_t *p;
  int addon;         /* a require's add-on: the runtime's own formats stay */
  char ns[256];      /* the class or module the walk is in, "" at the top */
} SpEntryWalk;

static void sp_entry_def(SpEntryWalk *w, const uint8_t *b, size_t n) {
  if (w->addon && n < 32) {
    char nm[32];
    memcpy(nm, b, n); nm[n] = '\0';
    if (is_time_native_format(nm)) return;
  }
  sp_names_add(w->s, n >= 2 && b[0] == '_' && b[1] == '_' ? "l:" : "m:", (const char *)b, n);
}

static bool sp_entry_visit(const pm_node_t *n, void *data) {
  SpEntryWalk *w = data;
  switch (PM_NODE_TYPE(n)) {
    case PM_CLASS_NODE: case PM_MODULE_NODE: {
      int is_class = PM_NODE_TYPE(n) == PM_CLASS_NODE;
      const pm_node_t *cpath = is_class ? ((const pm_class_node_t *)n)->constant_path
                                        : ((const pm_module_node_t *)n)->constant_path;
      const pm_node_t *body = is_class ? ((const pm_class_node_t *)n)->body : ((const pm_module_node_t *)n)->body;
      char own[256], full[256];
      if (!sp_names_path(w->p, cpath, own, sizeof own)) return false;
      snprintf(full, sizeof full, "%s%s%s", w->ns, w->ns[0] ? "::" : "", own);
      if (!is_native_class_name(full)) {
        sp_names_add(w->s, "c:", full, strlen(full));   /* created: reached by its constant */
        return false;
      }
      SpEntryWalk in = *w;
      snprintf(in.ns, sizeof in.ns, "%s", full);
      if (body) pm_visit_node(body, sp_entry_visit, &in);
      return false;
    }
    case PM_DEF_NODE: {
      const pm_constant_t *c = pm_constant_pool_id_to_constant(&w->p->constant_pool, ((const pm_def_node_t *)n)->name);
      sp_entry_def(w, c->start, c->length);
      return false;
    }
    case PM_ALIAS_METHOD_NODE: {
      const pm_node_t *nw = ((const pm_alias_method_node_t *)n)->new_name;
      if (PM_NODE_TYPE(nw) == PM_SYMBOL_NODE) {
        const pm_string_t *str = &((const pm_symbol_node_t *)nw)->unescaped;
        sp_entry_def(w, pm_string_source(str), pm_string_length(str));
      }
      return false;
    }
    case PM_CONSTANT_WRITE_NODE: {
      const pm_constant_t *c = pm_constant_pool_id_to_constant(&w->p->constant_pool, ((const pm_constant_write_node_t *)n)->name);
      char full[256];
      snprintf(full, sizeof full, "%s%s%.*s", w->ns, w->ns[0] ? "::" : "", (int)c->length, (const char *)c->start);
      sp_names_add(w->s, "c:", full, strlen(full));
      return true;
    }
    default: return true;
  }
}

/* A library a program may load at its start, or with a require it makes */
typedef struct {
  char lib[128];       /* as a require names it: "builtins/gem", "set" */
  char path[1024];     /* its file */
  int tier;            /* sp_lib_lookup's (SP_LIB_PACKAGE for an add-on) */
  const char *rides;   /* an add-on: the require that makes it available ("time") */
  SpNames entry;       /* its entry names */
  int loaded;
} SpBootLib;

/* A new slot at the end of `*libs` */
static SpBootLib *sp_boot_lib_slot(SpBootLib **libs, int n, int *cap) {
  if (n == *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *libs = realloc(*libs, sizeof **libs * (size_t)*cap);
    if (!*libs) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  return &(*libs)[n];
}

/* Library `lib`'s file, found as a require of it is (sp_lib_lookup), into
   a fresh `out`: 0 when there is none */
static int sp_boot_lib_find(SpLoadGraph *g, const char *lib, SpBootLib *out) {
  memset(out, 0, sizeof *out);
  snprintf(out->lib, sizeof out->lib, "%s", lib);
  out->tier = sp_lib_lookup(g->lib_dir, lib, out->path, sizeof out->path);
  return out->tier != SP_LIB_NONE;
}

/* Library `b`'s entry names, read from its file's tree: 0 when it cannot
   be read or does not parse */
static int sp_boot_lib_read(SpBootLib *b) {
  char *text = read_file(b->path);
  if (!text) return 0;
  pm_parser_t bp;
  sp_pm_parser_init(&bp, text, strlen(text), NULL);
  pm_node_t *br = pm_parse(&bp);
  SpEntryWalk w;
  memset(&w, 0, sizeof w);
  w.s = &b->entry; w.p = &bp; w.addon = b->rides != NULL;
  int ok = br && !bp.error_list.size;
  if (ok) pm_visit_node(br, sp_entry_visit, &w);
  pm_node_destroy(&bp, br);
  pm_parser_free(&bp);
  free(text);
  return ok;
}

/* The directory builtins/ is in, beside the compiler's lib/ or one level
   up (sp_lib_lookup's two places), into out; 0 when there is none */
static int sp_builtins_dir(const char *lib_dir, char *out, size_t n) {
  int base_len = (int)strlen(lib_dir);
  if (base_len >= 4 && strcmp(lib_dir + base_len - 4, "/lib") == 0) base_len -= 4;
  struct stat st;
  snprintf(out, n, "%.*s/builtins", base_len, lib_dir);
  if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
  snprintf(out, n, "%.*s/../builtins", base_len, lib_dir);
  return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

/* file `f`'s record, made the first time it is asked for. */
static SpGraphFile *sp_graph_rec(SpLoadGraph *g, int f) {
  if (f >= g->gfcap) {
    int cap = g->gfcap ? g->gfcap : 16;
    while (cap <= f) cap *= 2;
    SpGraphFile **v = realloc(g->gf, sizeof *v * (size_t)cap);
    if (!v) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    memset(v + g->gfcap, 0, sizeof *v * (size_t)(cap - g->gfcap));
    g->gf = v;
    g->gfcap = cap;
  }
  if (!g->gf[f] && !(g->gf[f] = calloc(1, sizeof **g->gf))) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  return g->gf[f];
}

/* ---- the files a computed require may name ----
 *
 * A require whose path is not a literal: the values its expression can take,
 * read from the tree with the parse-side evaluator (prism_eval.c) over the
 * file's directory. The locals it reads are bound first: a local assigned
 * one expression in its scope takes that expression's value, and the block
 * parameter of `%w(a b).each` takes each word in turn. One value is that
 * literal require. Several, or (for a plain require) the literals a path
 * built on a constant joins (the Mr Bones layout, sp_join_visit), which name
 * files beside this one, are a closed set of files (SP_LOAD_SET, with
 * candidates). A path not known is a set too, an open one: it may name any
 * file of the program. */
#define SP_REQ_MAX 16
/* unsure: a local the path reads may not hold its one value where the path
   is read: it escapes or is changed (sp_local_scan), or its write is not a
   plain statement of its scope */
typedef struct { char *v[SP_REQ_MAX]; int n, open, unsure, per_word; } SpPaths;   /* per_word: a path for each word of the loop the innermost scope is (sp_req_paths, sp_req_beside) */
typedef struct { const pm_parser_t *pp; const pm_node_t *const *stack; int depth; const char *dir; } SpReqCtx;

static void sp_paths_free(SpPaths *k) {
  for (int i = 0; i < k->n; i++) free(k->v[i]);
  k->n = 0;
}
static void sp_paths_open(SpPaths *k) { sp_paths_free(k); k->open = 1; k->per_word = 0; }
static void sp_paths_add(SpPaths *k, const char *s, size_t len) {
  if (k->n == SP_REQ_MAX) { sp_paths_open(k); return; }
  if (!(k->v[k->n] = strndup(s, len))) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  k->n++;
}

static int sp_scope_node(pm_node_type_t t) {
  return t == PM_BLOCK_NODE || t == PM_LAMBDA_NODE || t == PM_DEF_NODE || t == PM_CLASS_NODE ||
         t == PM_MODULE_NODE || t == PM_SINGLETON_CLASS_NODE || t == PM_PROGRAM_NODE;
}
static const pm_node_t *sp_scope_params(const pm_node_t *s) {
  switch (PM_NODE_TYPE(s)) {
    case PM_BLOCK_NODE: return ((const pm_block_node_t *)s)->parameters;
    case PM_LAMBDA_NODE: return ((const pm_lambda_node_t *)s)->parameters;
    case PM_DEF_NODE: return (const pm_node_t *)((const pm_def_node_t *)s)->parameters;
    default: return NULL;
  }
}
static const pm_node_t *sp_scope_body(const pm_node_t *s) {
  switch (PM_NODE_TYPE(s)) {
    case PM_BLOCK_NODE: return ((const pm_block_node_t *)s)->body;
    case PM_LAMBDA_NODE: return ((const pm_lambda_node_t *)s)->body;
    case PM_DEF_NODE: return ((const pm_def_node_t *)s)->body;
    case PM_PROGRAM_NODE: return (const pm_node_t *)((const pm_program_node_t *)s)->statements;
    case PM_CLASS_NODE: return ((const pm_class_node_t *)s)->body;
    case PM_MODULE_NODE: return ((const pm_module_node_t *)s)->body;
    case PM_SINGLETON_CLASS_NODE: return ((const pm_singleton_class_node_t *)s)->body;
    default: return NULL;
  }
}

/* Does the parameter list under `n` bind local `id`? */
typedef struct { pm_constant_id_t id; int found; } SpParamScan;
static bool sp_param_binds(const pm_node_t *n, void *data) {
  SpParamScan *p = data;
  pm_constant_id_t id = 0;
  switch (PM_NODE_TYPE(n)) {
    case PM_REQUIRED_PARAMETER_NODE: id = ((const pm_required_parameter_node_t *)n)->name; break;
    case PM_OPTIONAL_PARAMETER_NODE: id = ((const pm_optional_parameter_node_t *)n)->name; break;
    case PM_REST_PARAMETER_NODE: id = ((const pm_rest_parameter_node_t *)n)->name; break;
    case PM_REQUIRED_KEYWORD_PARAMETER_NODE: id = ((const pm_required_keyword_parameter_node_t *)n)->name; break;
    case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: id = ((const pm_optional_keyword_parameter_node_t *)n)->name; break;
    case PM_KEYWORD_REST_PARAMETER_NODE: id = ((const pm_keyword_rest_parameter_node_t *)n)->name; break;
    case PM_BLOCK_PARAMETER_NODE: id = ((const pm_block_parameter_node_t *)n)->name; break;
    case PM_BLOCK_LOCAL_VARIABLE_NODE: id = ((const pm_block_local_variable_node_t *)n)->name; break;
    case PM_LOCAL_VARIABLE_TARGET_NODE: id = ((const pm_local_variable_target_node_t *)n)->name; break;
    default: return true;
  }
  if (id == p->id) p->found = 1;
  return true;
}

/* The writes of one local of a scope: `lvl` soft scopes below it, a write
   names it by that depth. A plain write is counted (its value and the write
   kept); any other form that assigns it (a target, an operator write) is
   `other`. It `escapes` where the String it holds may change: a call on it
   but a reader of String's (`path << "x"`), and any read but such a
   receiver and `self`, the read being resolved (`alias = path`, `m(path)`). */
typedef struct {
  pm_constant_id_t name; int lvl, nwrite, other, escapes;
  const pm_node_t *val, *write, *self, *reader;
} SpLocalScan;
static bool sp_local_scan(const pm_node_t *n, void *data) {
  SpLocalScan *s = data;
  switch (PM_NODE_TYPE(n)) {
    case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE: return false;
    case PM_BLOCK_NODE: case PM_LAMBDA_NODE:
      s->lvl++; pm_visit_child_nodes(n, sp_local_scan, s); s->lvl--;
      return false;
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
      const pm_local_variable_write_node_t *w = (const pm_local_variable_write_node_t *)n;
      if (w->name == s->name && (int)w->depth == s->lvl) { s->nwrite++; s->val = w->value; s->write = n; }
      return true;
    }
    case PM_LOCAL_VARIABLE_READ_NODE: {
      const pm_local_variable_read_node_t *r = (const pm_local_variable_read_node_t *)n;
      if (r->name == s->name && (int)r->depth == s->lvl && n != s->self && n != s->reader) s->escapes = 1;
      return true;
    }
#define SP_LOCAL_OTHER(T, t) case T: \
      if (((const t *)n)->name == s->name && (int)((const t *)n)->depth == s->lvl) s->other = 1; \
      return true;
    SP_LOCAL_OTHER(PM_LOCAL_VARIABLE_TARGET_NODE, pm_local_variable_target_node_t)
    SP_LOCAL_OTHER(PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE, pm_local_variable_operator_write_node_t)
    SP_LOCAL_OTHER(PM_LOCAL_VARIABLE_OR_WRITE_NODE, pm_local_variable_or_write_node_t)
    SP_LOCAL_OTHER(PM_LOCAL_VARIABLE_AND_WRITE_NODE, pm_local_variable_and_write_node_t)
#undef SP_LOCAL_OTHER
    case PM_CALL_NODE: {
      const pm_call_node_t *c = (const pm_call_node_t *)n;
      const pm_local_variable_read_node_t *r = c->receiver && PM_NODE_TYPE(c->receiver) == PM_LOCAL_VARIABLE_READ_NODE ?
                                               (const pm_local_variable_read_node_t *)c->receiver : NULL;
      if (r && r->name == s->name && (int)r->depth == s->lvl) {
        const pm_constant_t *m = pm_constant_pool_id_to_constant(&g_parser->constant_pool, c->name);
        char name[32];
        if (m->length >= sizeof name) s->escapes = 1;
        else {
          memcpy(name, m->start, m->length);
          name[m->length] = '\0';
          if (!is_string_read_only_method(name)) s->escapes = 1;
          else s->reader = c->receiver;   /* (visited next) */
        }
      }
      return true;
    }
    default: return true;
  }
}

/* A block parameter of `%w(a b).each { |p| ... }`, or the first of
   `each_with_index { |p, i| ... }`: the words, and 1 when scope `si` is such
   a block whose parameter is `want` (any when 0); `got` is that parameter */
static int sp_each_words(const SpReqCtx *x, int si, pm_constant_id_t want, const pm_node_list_t **words, pm_constant_id_t *got) {
  const pm_node_t *s = x->stack[si];
  if (si < 1 || PM_NODE_TYPE(s) != PM_BLOCK_NODE || PM_NODE_TYPE(x->stack[si - 1]) != PM_CALL_NODE) return 0;
  const pm_call_node_t *c = (const pm_call_node_t *)x->stack[si - 1];
  const pm_node_t *bp = ((const pm_block_node_t *)s)->parameters;
  char name[16];
  if (c->block != s || c->arguments || !c->receiver || PM_NODE_TYPE(c->receiver) != PM_ARRAY_NODE ||
      !sp_name_buf(x->pp, c->name, name, sizeof name) || !is_each_or_index(name) || !bp || PM_NODE_TYPE(bp) != PM_BLOCK_PARAMETERS_NODE) return 0;
  int indexed = is_with_index_alias(name);   /* (each_with_index: the index is the second parameter) */
  const pm_parameters_node_t *pa = ((const pm_block_parameters_node_t *)bp)->parameters;
  const pm_array_node_t *list = (const pm_array_node_t *)c->receiver;
  if (!pa || pa->requireds.size != (indexed ? 2 : 1) || pa->optionals.size || pa->rest || pa->posts.size || pa->keywords.size ||
      pa->keyword_rest || pa->block || !list->elements.size ||
      PM_NODE_TYPE(pa->requireds.nodes[0]) != PM_REQUIRED_PARAMETER_NODE ||
      (indexed && PM_NODE_TYPE(pa->requireds.nodes[1]) != PM_REQUIRED_PARAMETER_NODE) ||
      (want && ((const pm_required_parameter_node_t *)pa->requireds.nodes[0])->name != want)) return 0;
  *got = ((const pm_required_parameter_node_t *)pa->requireds.nodes[0])->name;
  for (size_t i = 0; i < list->elements.size; i++)
    if (PM_NODE_TYPE(list->elements.nodes[i]) != PM_STRING_NODE) return 0;
  *words = &list->elements;
  return 1;
}

/* Is a path built as the Mr Bones layout builds one: literals, the loop's
   word, and calls on constants (`File.join(LIBPATH, "x", f)`,
   `Gem.libpath(["x", f])`), with at least one constant? */
typedef struct { int constants, bad; } SpShape;
static bool sp_shape_visit(const pm_node_t *n, void *data) {
  SpShape *s = data;
  switch (PM_NODE_TYPE(n)) {
    case PM_CONSTANT_READ_NODE: case PM_CONSTANT_PATH_NODE: s->constants++; return true;
    case PM_STRING_NODE: case PM_LOCAL_VARIABLE_READ_NODE: case PM_ARRAY_NODE: case PM_ARGUMENTS_NODE:
    case PM_CALL_NODE: return true;
    default: s->bad = 1; return false;
  }
}


/* The locals an expression reads (a block in it is not followed) */
typedef struct { const pm_local_variable_read_node_t *rd[8]; int n, bad; } SpReads;
static bool sp_reads_visit(const pm_node_t *n, void *data) {
  SpReads *r = data;
  if (PM_NODE_TYPE(n) == PM_BLOCK_NODE || PM_NODE_TYPE(n) == PM_LAMBDA_NODE) { r->bad = 1; return false; }
  if (PM_NODE_TYPE(n) != PM_LOCAL_VARIABLE_READ_NODE) return true;
  const pm_local_variable_read_node_t *rd = (const pm_local_variable_read_node_t *)n;
  for (int i = 0; i < r->n; i++) if (r->rd[i]->name == rd->name && r->rd[i]->depth == rd->depth) return true;
  if (r->n == 8) r->bad = 1;
  else r->rd[r->n++] = rd;
  return true;
}

/* Is `w` a statement of scope body `body` itself (not under a condition, a
   loop or a block)? */
static int sp_plain_stmt(const pm_node_t *body, const pm_node_t *w) {
  if (!body || !w || PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) return 0;
  const pm_node_list_t *l = &((const pm_statements_node_t *)body)->body;
  for (size_t i = 0; i < l->size; i++) if (l->nodes[i] == w) return 1;
  return 0;
}

/* Local `rd` of the require at `x`: its value bound in `ev` (a local assigned
   one expression in its scope, that expression's), or the words of the loop
   it is the parameter of. 0 when it is neither: any value. */
static int sp_req_bind(const SpReqCtx *x, PevCtx *ev, const pm_local_variable_read_node_t *rd,
                       const pm_node_list_t **words, pm_constant_id_t *wid, int *unsure) {
  int si = -1, hop = 0;
  for (int i = x->depth - 1; i >= 0 && si < 0; i--) {
    pm_node_type_t t = PM_NODE_TYPE(x->stack[i]);
    if (!sp_scope_node(t)) continue;
    if (hop++ == (int)rd->depth) si = i;
    else if (t != PM_BLOCK_NODE && t != PM_LAMBDA_NODE) break;
  }
  if (si < 0) return 0;
  const pm_node_t *s = x->stack[si], *params = sp_scope_params(s), *body = sp_scope_body(s);
  pm_constant_id_t pid = 0;
  int word = sp_each_words(x, si, rd->name, words, &pid);
  if (!word && params) {   /* a parameter, or a block-local, of the scope: any value */
    SpParamScan ps = { rd->name, 0 };
    pm_visit_node(params, sp_param_binds, &ps);
    if (ps.found) return 0;
  }
  SpLocalScan sc = { rd->name, 0, 0, 0, 0, NULL, NULL, (const pm_node_t *)rd, NULL };
  if (body) pm_visit_node(body, sp_local_scan, &sc);
  *unsure |= sc.escapes;
  if (sc.other || (word ? sc.nwrite != 0 : sc.nwrite != 1)) return 0;
  if (word) { *wid = pid; return 1; }
  *unsure |= !sp_plain_stmt(body, sc.write);
  PevCtx sub;   /* (the expression's own locals are not looked into) */
  memset(&sub, 0, sizeof sub);
  sub.dir = x->dir;
  Mv v;
  char *nm = cstr(rd->name);
  int ok = pev_eval(&sub, (pm_node_t *)sc.val, &v) && v.k == MV_STR;
  if (ok) pev_bind(ev, nm, v);
  free(nm);
  return ok;
}

/* The paths `arg` takes: one for each word of the loop it reads, or one.
   Open when a local it reads is no known value or the evaluator does not
   know the rest. */
static void sp_req_paths(const SpReqCtx *x, const pm_node_t *arg, SpPaths *out) {
  PevCtx ev;
  memset(&ev, 0, sizeof ev);
  ev.dir = x->dir;
  SpReads rs = { { NULL }, 0, 0 };
  pm_visit_node(arg, sp_reads_visit, &rs);
  const pm_node_list_t *words = NULL;
  pm_constant_id_t wid = 0;
  int ok = !rs.bad, inner = 0;
  for (int i = 0; ok && i < rs.n; i++) {
    const pm_node_list_t *w = NULL;
    pm_constant_id_t id = 0;
    ok = sp_req_bind(x, &ev, rs.rd[i], &w, &id, &out->unsure);
    if (ok && w) { ok = !words; words = w; wid = id; inner = rs.rd[i]->depth == 0; }   /* (depth 0: the loop is the innermost scope) */
  }
  char *wname = words ? cstr(wid) : NULL;
  for (size_t k = 0; ok && k < (words ? words->size : 1); k++) {
    Mv v;
    if (words) {
      const pm_string_t *u = &((const pm_string_node_t *)words->nodes[k])->unescaped;
      pev_bind(&ev, wname, mv_str((const char *)pm_string_source(u), pm_string_length(u), MV_STR));
    }
    if (pev_eval(&ev, (pm_node_t *)arg, &v) && v.k == MV_STR) sp_paths_add(out, v.s, strlen(v.s));
    else ok = 0;
  }
  if (!ok) sp_paths_open(out);
  else out->per_word = words && inner;
  free(wname);
  for (int i = 0; i < ev.nloc; i++) free(ev.loc[i].name);
}

/* The literals a computed path joins, for a require naming a file beside
   this one (sp_join_visit): one path for each word of the `%w(...).each`
   the require stands in, or one path. Open when the path is not built so,
   or joins no literal. */
static void sp_req_beside(const SpReqCtx *x, const pm_node_t *arg, SpPaths *out) {
  const pm_node_list_t *words = NULL;
  SpShape shape = { 0, 0 };
  pm_visit_node(arg, sp_shape_visit, &shape);
  if (shape.bad || !shape.constants) { sp_paths_open(out); return; }
  pm_constant_id_t pid = 0;
  int scope = x->depth - 1, wi = -1;   /* (the innermost scope, and the loop's) */
  while (scope >= 0 && !sp_scope_node(PM_NODE_TYPE(x->stack[scope]))) scope--;
  for (int i = x->depth - 1; i >= 0 && !words; i--)
    if (PM_NODE_TYPE(x->stack[i]) == PM_BLOCK_NODE && sp_each_words(x, i, 0, &words, &pid)) {
      wi = i;
      SpLocalScan sc = { pid, 0, 0, 0, 0, NULL };   /* (a word the block assigns anew is not the loop's) */
      pm_visit_node(sp_scope_body(x->stack[i]), sp_local_scan, &sc);
      if (sc.nwrite || sc.other) { sp_paths_open(out); return; }
    }
  /* A local the path reads other than the loop's word (the index of an
     each_with_index, any other local) has a value only the run time knows, so
     the literals alone name other files than the path does: it stays open. */
  SpReads rs = { { NULL }, 0, 0 };
  pm_visit_node(arg, sp_reads_visit, &rs);
  for (int i = 0; i < rs.n && !rs.bad; i++) rs.bad = !words || rs.rd[i]->name != pid;
  if (rs.bad) { sp_paths_open(out); return; }
  char *var = words ? cstr(pid) : NULL;
  for (size_t k = 0; k < (words ? words->size : 1) && !out->open; k++) {
    const pm_string_t *u = words ? &((const pm_string_node_t *)words->nodes[k])->unescaped : NULL;
    char *val = u ? strndup((const char *)pm_string_source(u), pm_string_length(u)) : NULL, joined[512];
    SpJoin j = { x->pp, var, val, joined, 0, sizeof joined, 0, 0 };
    joined[0] = 0;
    pm_visit_node(arg, sp_join_visit, &j);
    if (j.bad || !j.n) sp_paths_open(out);
    else {
      if (j.o > 3 && strcmp(joined + j.o - 3, ".rb") == 0) j.o -= 3;
      sp_paths_add(out, joined, j.o);
    }
    free(val);
  }
  out->per_word = !out->open && words && wi == scope;
  free(var);
}


/* Does the file's directory hold `path`? */
static int sp_beside_file(SpLoadGraph *g, int from, const char *path) {
  char found[1024];
  sp_load_rel_path(g, from, path, found, sizeof found);
  size_t fl = strlen(found);
  if (fl < sizeof found - 4 && (fl < 3 || strcmp(found + fl - 3, ".rb") != 0)) strcat(found, ".rb");
  return sp_lib_file(found);
}

/* The bare features that name file `f`, newline-separated (NULL for none):
   the ends of its path, from a '/', that a literal `require` of resolves to
   it (sp_lib_lookup, the one rule, so the load path's order decides which
   file a feature names), and the directory's, where the lookup reads
   <root>/X/X.rb as X. Those the compiler's own roots answer (lib/, builtins/,
   packages/): a computed bare path names a file by exactly these at the
   compiler's root of the run-time $LOAD_PATH. A file under a root from -I is
   named by that root's entry there, which sp_require_search matches by the
   file's path. */
static char *sp_bare_features(SpLoadGraph *g, int f) {
  char *a = strdup(g->file[f]), *list = NULL;
  if (!a) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t al = strlen(a), ll = 0;
  if (al > 3 && strcmp(a + al - 3, ".rb") == 0) a[al -= 3] = 0;
  const char *last = strrchr(a, '/');
  for (int dirpass = 0; dirpass < 2; dirpass++) {
    if (dirpass) {   /* X for <root>/X/X */
      size_t ln = last ? strlen(last + 1) : 0;
      if (!ln || al < 2 * ln + 2 || a[al - ln - 1] != '/' || a[al - 2 * ln - 2] != '/' || strncmp(a + al - 2 * ln - 1, a + al - ln, ln) != 0) break;
      a[al - ln - 1] = 0;
    }
    for (const char *t = strchr(a, '/'); t; t = strchr(t + 1, '/')) {
      char found[1024], *canon;
      int tier = t[1] ? sp_lib_lookup(g->lib_dir, t + 1, found, sizeof found) : SP_LIB_NONE;
      /* (a file under a root from -I is named by that entry of the run-time $LOAD_PATH, not by the compiler's root) */
      if (tier == SP_LIB_NONE || tier == SP_LIB_ROOT) continue;
      int same = (canon = sp_canonical_path(found)) && strcmp(canon, g->file[f]) == 0;
      free(canon);
      if (!same) continue;
      size_t tl = strlen(t + 1);
      list = realloc(list, ll + tl + 2);
      if (!list) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      memcpy(list + ll, t + 1, tl); ll += tl; list[ll++] = '\n'; list[ll] = 0;
    }
  }
  free(a);
  return list;
}

/* The directories -I names as File.expand_path gives them (against the
   current directory, "." and ".." folded, a symlink left as written), one
   per line and in order: the first entries of the run-time $LOAD_PATH, as
   CRuby's start with them (sp_load_path_seed). NULL for none. */
static char *sp_seed_dirs(const SpLoadGraph *g) {
  char *list = NULL;
  size_t ll = 0;
  for (int i = 0; i < g->niroots; i++) {
    char cwd[1024], full[2048];
    const char *r = g->iroots[i];
    if (r[0] != '/' && getcwd(cwd, sizeof cwd)) snprintf(full, sizeof full, "%s/%s", cwd, r);
    else snprintf(full, sizeof full, "%s", r);
    sp_normalize_dots(full);
    size_t dl = strlen(full);
    list = realloc(list, ll + dl + 2);
    if (!list) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    memcpy(list + ll, full, dl); ll += dl; list[ll++] = '\n'; list[ll] = 0;
  }
  return list;
}

/* The requires of a file's tree, in the tree's order, each resolved
   (sp_load_resolve), noted with the top-level statement it stands in, and
   kept with its call node, which flatten's LoadNode looks it up by
   (sp_graph_site_take). `stack` holds the nodes above the one visited,
   from the file's root. */
typedef struct { SpLoadGraph *g; int f, stmt; const pm_node_t *top; const pm_node_t **stack; int depth, cap; SpFileReturn ret; const SpPreFileCtx *pre; int in_bo; } SpSiteWalk;

/* The edge of a require whose path is no literal: the one file a known path
   names, as that literal does; the files a known set or the files beside
   this one name (SP_LOAD_SET); or open (SP_LOAD_SET too, `open`: any file
   of the program). */
/* Does the file an autoload of `path` from file `from` names parse? 1 when
   it names none, which the require edge reports as a require would. */
static int sp_load_parses(SpLoadGraph *g, int from, const char *path) {
  char found[1024];
  int tier;
  char *text = sp_load_find(g, from, path[0] == '/', path, found, sizeof found, &tier) ? read_file(found) : NULL;
  if (!text) return 1;
  SpSrcFile s;
  sp_src_file_parse(&s, text, found);
  int ok = s.parser.error_list.size == 0;
  sp_src_file_free(&s);
  free(text);
  return ok;
}

/* The literal head and tail of an interpolated path (`"x/#{v}/n"`: "x/"
   and "/n"), kept on edge e: the parts before the first part only the run
   time knows and after the last, a require_relative's head (from, the
   requiring file) made absolute. Neither is kept when both are empty, or
   for a head a plain require reads against the current directory. */
static char *sp_pm_str_parts(const pm_node_list_t *ps, size_t from, size_t to) {
  size_t len = 0;
  for (size_t i = from; i < to; i++) len += pm_string_length(&((const pm_string_node_t *)ps->nodes[i])->unescaped);
  char *out = malloc(len + 1), *o = out;
  if (!out) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (size_t i = from; i < to; i++) {
    const pm_string_t *u = &((const pm_string_node_t *)ps->nodes[i])->unescaped;
    memcpy(o, pm_string_source(u), pm_string_length(u));
    o += pm_string_length(u);
  }
  *o = 0;
  return out;
}
static void sp_req_pattern(const pm_node_t *arg, const char *from, SpLoadEdge *e) {
  if (PM_NODE_TYPE(arg) != PM_INTERPOLATED_STRING_NODE) return;
  const pm_node_list_t *ps = &((const pm_interpolated_string_node_t *)arg)->parts;
  size_t h = 0, t = ps->size;
  while (h < ps->size && PM_NODE_TYPE(ps->nodes[h]) == PM_STRING_NODE) h++;
  while (t > h && PM_NODE_TYPE(ps->nodes[t - 1]) == PM_STRING_NODE) t--;
  if (h == ps->size) return;
  char *pre = sp_pm_str_parts(ps, 0, h), *suf = sp_pm_str_parts(ps, t, ps->size);
  size_t sl = strlen(suf);
  if (sl >= 3 && strcmp(suf + sl - 3, ".rb") == 0) suf[sl - 3] = 0;
  if (!pre[0] && !suf[0]) { free(pre); free(suf); return; }
  /* a require_relative's head is read from the requiring file's directory */
  if (from && pre[0] != '/') {
    const char *sl = strrchr(from, '/');
    size_t dl = sl ? (size_t)(sl - from) : 0, hl = strlen(pre);
    char *abs = malloc(dl + hl + 2);
    if (!abs) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    memcpy(abs, from, dl);
    abs[dl] = '/';
    memcpy(abs + dl + 1, pre, hl + 1);
    sp_normalize_dots(abs);
    free(pre);
    pre = abs;
  }
  else if (pre[0] == '.' || pre[0] == '~') { free(pre); free(suf); return; }
  e->pat_pre = pre;
  e->pat_suf = suf;
}
/* Can a require of a path with head pre and tail suf, whatever comes
   between, name file `path` (".rb" taken off) from some directory of the
   run-time $LOAD_PATH: path ends in suf, and has pre at its start for an
   absolute head, else after a '/', before that tail (a '/' they both have
   at the join may be one: "x//n" names x/n) */
static int sp_req_pattern_names(const char *pre, const char *suf, const char *path) {
  size_t pl = strlen(path), sl = strlen(suf), hl = strlen(pre);
  if (pl >= 3 && strcmp(path + pl - 3, ".rb") == 0) pl -= 3;
  if (sl > pl || strncmp(path + pl - sl, suf, sl) != 0) return 0;
  size_t tail = pl - sl;
  int join = hl && pre[hl - 1] == '/' && sl && suf[0] == '/';
  if (!hl) return 1;
  if (pre[0] == '/') return hl <= tail + (size_t)join && strncmp(path, pre, hl) == 0;
  for (size_t i = 0; i + hl <= tail + (size_t)join; i++)
    if (path[i] == '/' && i + 1 + hl <= pl && strncmp(path + i + 1, pre, hl) == 0 && i + 1 + hl <= tail + (size_t)join) return 1;
  return 0;
}

static int sp_graph_computed(SpSiteWalk *w, const pm_call_node_t *n, const char *method, int line) {
  SpLoadGraph *g = w->g;
  char *dir = strdup(g->file[w->f]), *sl = dir ? strrchr(dir, '/') : NULL;
  if (!dir) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (sl) *sl = 0;
  SpReqCtx x = { g_parser, w->stack, w->depth, dir };
  int auto_load = sp_pm_name_is(g_parser, n->name, "autoload");
  const pm_node_t *arg = n->arguments->arguments.nodes[auto_load ? 1 : 0];
  SpPaths kv = { { NULL }, 0, 0 };
  int plain = auto_load || strcmp(method, "require") == 0, beside = 0, e;
  sp_req_paths(&x, arg, &kv);
  if (kv.open && plain) {
    sp_paths_free(&kv); kv.open = 0; beside = 1;
    sp_req_beside(&x, arg, &kv);
  }
  free(dir);
  /* an autoload's file runs only at its constant's first reference: one
     that does not parse is no file of the graph, and the call makes no
     registration (the program layer decides what it is) */
  int nfile = 0, to[SP_REQ_MAX], rel = 0, bad = kv.open || (auto_load && !beside && kv.n == 1 && !sp_load_parses(g, w->f, kv.v[0]));
  int every = 1;   /* (each path names a file of the program) */
  const char *one = NULL;
  for (int i = 0; i < kv.n && !bad; i++) {
    const char *p = kv.v[i];
    rel = beside || !plain || p[0] == '/';
    int f = -1, how = SP_LOAD_NONE;
    /* (a path beside the file is looked up only where the file is: the
       features a require names are noted as it resolves) */
    if (!rel || sp_beside_file(g, w->f, p)) how = sp_load_target(g, w->f, rel, p, &f);
    one = p;
    if (how != SP_LOAD_FILE) { every = 0; continue; }
    int dup = 0;
    for (int k = 0; k < nfile; k++) dup |= to[k] == f;
    if (!dup) to[nfile++] = f;
  }
  if (!bad && !(beside && !nfile) && !beside && kv.n == 1) {   /* one path is known: that literal */
    e = sp_load_resolve(g, w->f, auto_load ? "require" : method, one, line, rel);
  }
  else {
    sp_load_edge(&g->edge, &g->nedge, &g->edgecap, w->f, line, plain, "(computed)", SP_LOAD_SET, -1);
    e = g->nedge - 1;
    g->edge[e].open = bad || (beside && !nfile);
    if (g->edge[e].open && !auto_load) sp_req_pattern(arg, plain ? NULL : g->file[w->f], &g->edge[e]);
    g->edge[e].ncand = g->edge[e].open ? 0 : nfile;
    g->edge[e].cand = g->edge[e].ncand ? malloc(sizeof(int) * (size_t)nfile) : NULL;
    for (int k = 0; k < g->edge[e].ncand; k++) g->edge[e].cand[k] = to[k];
    g->edge[e].per_word = !g->edge[e].open && !auto_load && kv.per_word && every;
    sp_load_resolved(g, w->f);
  }
  if (auto_load) {
    /* (a require's warning is the program's, which knows whether a rescue
       catches the LoadError: prog_settle_files) */
    if (g->edge[e].how == SP_LOAD_NONE)
      fprintf(stderr, "warning: %s:%d: cannot load such file -- %s (the require raises LoadError when it runs)\n",
              g->file[w->f], line, g->edge[e].name);
    SpPaths names = { { NULL }, 0, 0 };
    const pm_node_t *name = n->arguments->arguments.nodes[0];
    if (PM_NODE_TYPE(name) == PM_SYMBOL_NODE) {
      const pm_string_t *u = &((const pm_symbol_node_t *)name)->unescaped;
      sp_paths_add(&names, (const char *)pm_string_source(u), pm_string_length(u));
    }
    else sp_req_paths(&x, name, &names);
    SpLoadEdge *edge = &g->edge[e];
    /* (a local the path or the name reads may hold another value where the
       registration runs: nil past a write that did not run, the String a
       call left; the registration stays open) */
    edge->nauto = names.open || bad || beside || kv.n != 1 || nfile != 1 || kv.unsure || names.unsure ? 0 : names.n;
    edge->autoload = edge->nauto ? calloc((size_t)edge->nauto, sizeof(char *)) : NULL;
    edge->auto_path = edge->nauto ? calloc((size_t)edge->nauto, sizeof(char *)) : NULL;
    edge->auto_file = edge->nauto ? calloc((size_t)edge->nauto, sizeof(int)) : NULL;
    if (edge->nauto && (!edge->autoload || !edge->auto_path || !edge->auto_file)) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    for (int k = 0; k < edge->nauto; k++) {
      edge->autoload[k] = names.v[k];
      names.v[k] = NULL;
      const char *path = kv.v[0];   /* (one path is known, or there is no registration) */
      edge->auto_file[k] = -1;
      sp_load_target(g, w->f, path[0] == '/', path, &edge->auto_file[k]);
      edge->auto_path[k] = strdup(path);
      if (!edge->auto_path[k]) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    sp_paths_free(&names);
  }
  sp_paths_free(&kv);
  return e;
}

static bool sp_graph_site_visit(const pm_node_t *node, void *data) {
  SpSiteWalk *w = data;
  if (PM_NODE_TYPE(node) == PM_CLASS_NODE) {
    const pm_class_node_t *n = (const pm_class_node_t *)node;
    if (w->depth == w->cap) {
      w->cap = w->cap ? w->cap * 2 : 32;
      w->stack = realloc(w->stack, sizeof *w->stack * (size_t)w->cap);
      if (!w->stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    w->stack[w->depth++] = node;
    if (n->constant_path) pm_visit_node(n->constant_path, sp_graph_site_visit, w);
    if (n->superclass) pm_visit_node(n->superclass, sp_graph_site_visit, w);
    int is_bo = sp_pre_pm_is_basic_object(g_parser, n->superclass);
    w->in_bo += is_bo;
    if (n->body) pm_visit_node(n->body, sp_graph_site_visit, w);
    w->in_bo -= is_bo;
    w->depth--;
    return false;
  }
  if (PM_NODE_TYPE(node) == PM_IN_NODE) {
    const pm_in_node_t *n = (const pm_in_node_t *)node;
    if (w->depth == w->cap) {
      w->cap = w->cap ? w->cap * 2 : 32;
      w->stack = realloc(w->stack, sizeof *w->stack * (size_t)w->cap);
      if (!w->stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    w->stack[w->depth++] = node;
    const SpPreFileCtx *saved = w->pre;
    w->pre = NULL;
    if (n->pattern) pm_visit_node(n->pattern, sp_graph_site_visit, w);
    w->pre = saved;
    if (n->statements) pm_visit_node((const pm_node_t *)n->statements, sp_graph_site_visit, w);
    w->depth--;
    return false;
  }
  if (w->pre && (PM_NODE_TYPE(node) == PM_IF_NODE || PM_NODE_TYPE(node) == PM_UNLESS_NODE)) {
    int is_unless = PM_NODE_TYPE(node) == PM_UNLESS_NODE;
    const pm_node_t *pred = is_unless ? ((const pm_unless_node_t *)node)->predicate : ((const pm_if_node_t *)node)->predicate;
    const pm_node_t *body = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->statements
                                      : (const pm_node_t *)((const pm_if_node_t *)node)->statements;
    const pm_node_t *other = is_unless ? (const pm_node_t *)((const pm_unless_node_t *)node)->else_clause
                                       : ((const pm_if_node_t *)node)->subsequent;
    int tr = sp_pre_pm_truth(g_parser, pred, w->pre, !w->in_bo, 0);
    if (tr >= 0) {
      if (w->depth == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 32;
        w->stack = realloc(w->stack, sizeof *w->stack * (size_t)w->cap);
        if (!w->stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      }
      w->stack[w->depth++] = node;
      const pm_node_t *live = (is_unless ? !tr : tr) ? body : other;
      if (live) pm_visit_node(live, sp_graph_site_visit, w);
      w->depth--;
      return false;
    }
  }
  if (PM_NODE_TYPE(node) == PM_CALL_NODE && sp_load_call((const pm_call_node_t *)node)) {
    SpGraphFile *rf = sp_graph_rec(w->g, w->f);
    for (int si = 0; si < rf->nsite; si++)
      if (rf->site[si].call == node) goto visit_children;
    const pm_call_node_t *n = (const pm_call_node_t *)node;
    int auto_load = sp_pm_name_is(g_parser, n->name, "autoload");
    const pm_node_t *arg = n->arguments->arguments.nodes[auto_load ? 1 : 0];
    char *method = cstr(n->name), *path = NULL;
    int line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line), e;
    if (!auto_load && PM_NODE_TYPE(arg) == PM_STRING_NODE) {
      const pm_string_t *u = &((const pm_string_node_t *)arg)->unescaped;
      path = strndup((const char *)pm_string_source(u), pm_string_length(u));
      if (!path) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      e = sp_load_resolve(w->g, w->f, method, path, line, 0);
    }
    else e = sp_graph_computed(w, n, method, line);
    w->g->edge[e].stmt = w->stmt;
    w->g->edge[e].top = !auto_load && node == w->top;
    w->g->edge[e].begin_block = PM_NODE_TYPE(w->top) == PM_PRE_EXECUTION_NODE;
    SpGraphFile *r = sp_graph_rec(w->g, w->f);
    if (r->nsite == r->sitecap) {
      r->sitecap = r->sitecap ? r->sitecap * 2 : 8;
      r->site = realloc(r->site, sizeof *r->site * (size_t)r->sitecap);
      if (!r->site) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    }
    r->site[r->nsite++] = (SpGraphSite){ e, node, 0, !auto_load && !sp_req_always_runs(w->stack, w->depth, node, &w->ret) };
    free(method);
    free(path);
  }
visit_children:
  if (w->depth == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 32;
    w->stack = realloc(w->stack, sizeof *w->stack * (size_t)w->cap);
    if (!w->stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  w->stack[w->depth++] = node;
  pm_visit_child_nodes(node, sp_graph_site_visit, w);
  w->depth--;
  return false;
}

/* The load path as the program has made it by the statement the walk is at:
   all of it, in order (`dirs`: the roots from -I with the directories its
   statements have put before and after them), which a file loaded from there
   starts with; NULL for the roots from -I alone. */
static void sp_roots_set(const SpLoadGraph *g, char *const *dirs, int n) {
  for (int i = 0; i < sp_feature_roots_n; i++) free(sp_feature_roots[i]);
  sp_feature_roots_n = 0;
  if (!dirs) { dirs = g->iroots; n = g->niroots; }
  for (int i = 0; i < n; i++) sp_feature_root_at(dirs[i], 0);
}
/* File `f`, named just now by a require of the file being read: it starts
   with the load path as that file has it. */
static void sp_graph_inherit(SpLoadGraph *g, int f) {
  SpGraphFile *r = sp_graph_rec(g, f);
  int n = sp_feature_roots_n;
  r->dirs = n > 0 ? malloc(sizeof *r->dirs * (size_t)n) : NULL;
  if (n > 0 && !r->dirs) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int i = 0; i < n; i++) {
    r->dirs[i] = strdup(sp_feature_roots[i]);
    if (!r->dirs[i]) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  r->ndirs = n;
}
/* Each parse error at its position, `file:line:col: message` (the column
   1-based on stderr, as an editor reads a compiler's message; 0-based in
   the JSON, as Prism gives it and --emit-types carries it). The file is
   `shown` when given (a file of the load graph, parsed as written), else
   the multi-file map's for the line, so an error in a required file names
   that file. Under --emit-types the JSON is written too, with no types and
   the errors as its diagnostics: a buffer is unparseable most of the time
   it is being typed into, and its marker belongs on the token, not on
   line 1. */
static void sp_report_parse_errors(pm_parser_t *parser, const char *source_file, const char *shown) {
  fprintf(stderr, "Parse errors in '%s':\n", source_file);
  const char *types_out = g_opt_emit_types;
  FILE *jf = types_out ? fopen(types_out, "w") : NULL;
  if (types_out && !jf) fprintf(stderr, "spinel: cannot write '%s'\n", types_out);
  if (jf) fputs("{\n  \"types\": [\n\n  ],\n  \"diagnostics\": [\n", jf);
  int nd = 0;
  pm_diagnostic_t *diag;
  for (diag = (pm_diagnostic_t *)parser->error_list.head; diag; diag = (pm_diagnostic_t *)diag->node.next) {
    pm_line_column_t lc = pm_newline_list_line_column(&parser->newline_list, diag->location.start, parser->start_line);
    pm_line_column_t le = pm_newline_list_line_column(&parser->newline_list, diag->location.end, parser->start_line);
    const char *file = shown ? shown : source_file;
    int line = lc.line, end_line = le.line;
    if (!shown && sp_line_map_n > 0 && lc.line >= 1 && lc.line <= sp_line_map_n && sp_line_orig[lc.line] > 0) {
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
}

/* Each warning of the parse of a file of the program's own, as CRuby
   writes it when it reads the file: the ones of the default level always,
   the ones ruby -w adds (an unused local, a statement not reached, ...)
   under -w. A compiled binary reads no source when it runs, so the compile
   is where a slip such as a duplicated Hash key can be told. The text of
   -e is named -e, as CRuby names it. */
static void sp_report_parse_warnings(const SpSrcFile *f) {
  const pm_parser_t *parser = &f->parser;
  const char *file = f->shown ? f->shown : f->path;
  if (g_opt_eval_path && strcmp(f->path, g_opt_eval_path) == 0) file = "-e";
  for (const pm_diagnostic_t *d = (const pm_diagnostic_t *)parser->warning_list.head; d;
       d = (const pm_diagnostic_t *)d->node.next) {
    if (d->level == PM_WARNING_LEVEL_VERBOSE && !g_opt_warn_verbose) continue;
    int32_t line = pm_newline_list_line(&parser->newline_list, d->location.start, parser->start_line);
    fprintf(stderr, "spinel: %s:%d: warning: %s\n", file, (int)line, d->message);
  }
}

static void sp_graph_scan_sites(SpLoadGraph *g, int f, const char *path, SpNames *facts, const SpPreFileCtx *pre) {
  SpGraphFile *r = sp_graph_rec(g, f);
  sp_roots_set(g, r->dirs, r->ndirs);
  g_parser = &r->src.parser;
  const pm_node_t *root = r->src.root;
  const pm_statements_node_t *st = PM_NODE_TYPE_P(root, PM_PROGRAM_NODE) ? ((const pm_program_node_t *)root)->statements : NULL;
  SpSiteWalk w = { g, f, 0, NULL, NULL, 0, 0, { NULL, 0 }, pre, 0 };
  if (st) {
    w.stack = malloc(sizeof *w.stack * (size_t)(w.cap = 32));
    if (!w.stack) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    w.stack[w.depth++] = root;
  }
  char *dir = sp_file_real_dir(path);
  int builtin = (*sp_load_flags(g, f) & SP_FILE_BUILTIN) != 0;
  for (size_t i = 0; st && i < st->body.size; i++) {
    w.stmt = (int)i; w.top = st->body.nodes[i];
    pm_visit_node(st->body.nodes[i], sp_graph_site_visit, &w);
    /* a directory the statement puts on the load path is one for the statements after it */
    if (dir && !builtin && sp_load_path_stmt(&r->src.parser, st->body.nodes[i], dir, 0))
      sp_load_path_stmt(&r->src.parser, st->body.nodes[i], dir, 1);
  }
  free(dir);
  free(w.stack);
  sp_names_of(facts, &r->src.parser, r->src.root, pre);
}

/* file `f` of graph `g` read and parsed, before any
   file is flattened: its requires resolved (each target file a file of the
   graph, read in its turn) and the names it uses added to `facts`. 0 when it
   could not be read or parsed (reported). */
static int sp_graph_read(SpLoadGraph *g, int f, const char *source_file, SpNames *facts) {
  const char *path = f ? g->file[f] : source_file;
  /* (a library of the compiler's has its declarations appended, by the
     tier the require that found the file gave it, and beside the path the
     require found it at: a package file that is a symlink has its
     declarations beside the link) */
  char *text = sp_lib_text_appended(read_file(path), f ? g->found[f] : source_file, sp_load_tier(g, f));
  if (!text) { fprintf(stderr, "spinel_parse: cannot open '%s'\n", path); return 0; }
  SpGraphFile *r = sp_graph_rec(g, f);
  r->text = text;
  sp_src_file_parse(&r->src, text, path);
  if (f && sp_load_tier(g, f) == SP_LIB_NONE) r->src.shown = sp_shown_path(path, source_file);   /* (the entry is named as given) */
  r->read = 1;
  sp_src_file_note_defers(&r->src);
  sp_src_file_warn_encoding(&r->src);
  if (r->src.parser.error_list.size) {
    sp_report_parse_errors(&r->src.parser, source_file, r->src.shown ? r->src.shown : path);
    return 0;
  }
  sp_src_file_fsl(&r->src);   /* (its pragma's warning; every literal is frozen) */
  if (sp_load_tier(g, f) == SP_LIB_NONE && !(*sp_load_flags(g, f) & SP_FILE_BUILTIN))
    sp_report_parse_warnings(&r->src);
  if (!(*sp_load_flags(g, f) & SP_FILE_BUILTIN)) {
    sp_pre_scan_file(&r->src.parser, r->src.root, g->has_pre_inherit ? &g->pre_inherit : NULL, &r->pre);
    r->has_pre = 1;
    if (sp_pre_ctx_merge(&g->pre_inherit, &r->pre)) g->pre_tightened = 1;
    g->has_pre_inherit = 1;
  }
  sp_graph_scan_sites(g, f, path, facts, g->has_pre_inherit ? &g->pre_inherit : NULL);
  return 1;
}

/* the file sp_graph_flatten is flattening, whose sites
   its LoadNodes look themselves up in, and the site after the last one
   taken. */
typedef struct { SpLoadGraph *g; SpGraphFile *r; int next; } SpGraphFlat;
static SpGraphFlat g_flat_sites;

/* LoadNode `id`, flattened from require `call` of the
   file being flattened, carries what the call's site resolved to. The site
   is found by its call node, not by its place: flatten meets a file's
   requires in its own order, which is not the tree's everywhere (a begin's
   ensure comes before its else), and two requires on one line differ only
   by their node. The search starts at the site after the last one taken,
   where the next one nearly always is. 1 when the path is computed when
   the require runs (its LoadNode keeps the expression). */
static int sp_graph_site_take(int id, const pm_node_t *call) {
  static const char *const how_name[] = { "file", "native", "core", "none", "run-time", "set" };   /* (SP_LOAD_*; no LoadNode is run-time) */
  SpGraphFile *r = g_flat_sites.r;
  for (int i = 0; r && i < r->nsite; i++) {
    int k = (g_flat_sites.next + i) % r->nsite;
    if (r->site[k].call != call) continue;
    r->site[k].taken++;
    g_flat_sites.next = k + 1;
    SpLoadEdge *e = &g_flat_sites.g->edge[r->site[k].edge];
    e->node = id;
    for (int k = 0; k < e->nauto; k++)
      nt_add_autoload(out_nt, id, e->auto_file[k], e->autoload[k], e->auto_path[k]);
    if (sp_pm_name_is(g_parser, ((const pm_call_node_t *)call)->name, "autoload")) return 0;
    if (r->site[k].deferred) emit_int(id, "req_deferred", 1);
    emit_str(id, "how", how_name[e->how]);
    if (e->to >= 0) emit_int(id, "target_file", e->to);
    /* the path a computed expression names, when it names one */
    if (e->how != SP_LOAD_SET &&
        PM_NODE_TYPE(((const pm_call_node_t *)call)->arguments->arguments.nodes[0]) != PM_STRING_NODE)
      emit_str(id, "path", e->name);
    /* a set: the files it may name (prog_load_cand), or any (open) */
    if (e->how == SP_LOAD_SET && e->open && e->pat_pre) {
      SpLoadGraph *g = g_flat_sites.g;
      free(e->cand);
      e->cand = malloc(sizeof(int) * (size_t)(g->nfile + 1));
      if (!e->cand) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      e->ncand = 0;
      for (int f = 0; f < g->nfile; f++)
        if (sp_req_pattern_names(e->pat_pre, e->pat_suf, g->file[f])) e->cand[e->ncand++] = f;
      e->open = 0;
    }
    if (e->how == SP_LOAD_SET) {
      emit_int(id, "ncand", e->ncand);
      if (e->open) emit_int(id, "open", 1);
      if (e->per_word) emit_int(id, "per_word", 1);
      char *dir = strdup(g_flat_sites.g->file[e->from]), *sl = dir ? strrchr(dir, '/') : NULL;   /* the requiring file's directory */
      if (!dir) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      if (sl) *sl = 0;
      emit_str(id, "dir", dir);
      free(dir);
      for (int c = 0; c < e->ncand; c++) {
        char key[32];
        snprintf(key, sizeof key, "cand_%d", c);
        emit_int(id, key, e->cand[c]);
      }
    }
    /* one that names no file: the feature its LoadError names, the path
       as written for a require, from the file's directory for a
       require_relative */
    if (e->how == SP_LOAD_NONE) {
      char feature[1024];
      if (e->plain) snprintf(feature, sizeof feature, "%s", e->name);
      else sp_load_rel_path(g_flat_sites.g, e->from, e->name, feature, sizeof feature);
      emit_str(id, "feature", feature);
    }
    return e->how == SP_LOAD_SET;
  }
  return 0;
}

/* file `f` of graph `g`, read before (sp_graph_read),
   flattened into the table as a root of its own (its ProgramNode carries
   its graph id, `load_file`; the entry's is the table's root), each of its
   requires a LoadNode carrying what it loads, and its inline RBS read from
   its own comments. 0 on an error in an annotation or an internal error
   (reported). */
static int sp_graph_flatten(SpLoadGraph *g, int f, const char *source_file) {
  SpGraphFile *r = sp_graph_rec(g, f);
  /* the file in the table's file list, in graph order: flatten stamps a
     node's position from this parse, and a builtins/ root's nodes as
     builtin ones; a file is named as a message names it (sp_shown_path),
     its #line directives too */
  g_flat_file = sp_intern_file(f ? (r->src.shown ? r->src.shown : g->file[f]) : source_file, f ? sp_load_tier(g, f) : SP_LIB_NONE);
  g_flat_builtin = (*sp_load_flags(g, f) & SP_FILE_BUILTIN) != 0;
  g_bi_base = -1;
  g_parser = &r->src.parser;
  g_source_file = source_file;
  g_owner = NULL;
  /* inline RBS over the file's own comment list, its facts stamped on the
     file's own nodes as flatten makes them */
  if (sp_inline_rbs_run(&r->src.parser, r->src.root, r->text, strlen(r->text), sp_file_table[g_flat_file], g_flat_file) > 0) {
    sp_inline_rbs_failed = 1;
    sp_inline_rbs_free();
    return 0;
  }
  /* a `#<SPINEL_SOURCE>file:line` marker in the file pins the positions
     after it: the file's own map for its parse, the file's own lines
     elsewhere */
  int saved_map_n = sp_line_map_n;
  int *saved_lf = sp_line_file, *saved_lo = sp_line_orig;
  int *saved_df = sp_disp_file, *saved_dl = sp_disp_line;
  int own_map = r->text && strstr(r->text, SP_SOURCE_PREFIX) != NULL;
  if (own_map) sp_build_line_map(r->text, sp_file_table[g_flat_file]);
  g_flat_sites = (SpGraphFlat){ g, r, 0 };
  g_flat_pre = g->has_pre_inherit ? &g->pre_inherit : (r->has_pre ? &r->pre : NULL);
  g_in_pattern = 0;
  g_in_basic_object = 0;
  int start = node_counter, root = flatten(r->src.root);
  g_flat_pre = NULL;
  g_in_pattern = 0;
  g_in_basic_object = 0;
  g_flat_sites = (SpGraphFlat){ NULL, NULL, 0 };
  if (own_map) {
    free(sp_line_file); free(sp_line_orig); free(sp_disp_file); free(sp_disp_line);
    sp_line_map_n = saved_map_n;
    sp_line_file = saved_lf; sp_line_orig = saved_lo;
    sp_disp_file = saved_df; sp_disp_line = saved_dl;
  }
  sp_inline_rbs_done();
  sp_inline_rbs_free();
  if (!f) out_nt->root_id = root;
  r->root = root;
  emit_int(root, "load_file", f);
  emit_str(root, "file_path", g->file[f]);
  int any_set = 0;   /* the bare features a computed require names it by (prog_load_cand) */
  for (int e = 0; e < g->nedge; e++) any_set |= g->edge[e].how == SP_LOAD_SET;
  if (any_set && f > 0) {
    char *bare = sp_bare_features(g, f);
    if (bare) emit_str(root, "bare_features", bare);
    free(bare);
  }
  if (!f) {   /* the run-time $LOAD_PATH as CRuby's starts: -I's directories, then the compiler's feature root */
    char *dirs = sp_seed_dirs(g);
    /* the library directory, as sp_lib_lookup finds lib/: <exe dir>/lib, or one level up
       when the compiler binary is in bin/ or build/ (then <exe dir>/lib is no directory) */
    char root_dir[1100];
    char *lib = realpath(g->lib_dir, NULL);
    if (!lib) {
      snprintf(root_dir, sizeof root_dir, "%.*s/../lib", strlen(g->lib_dir) > 4 ? (int)(strlen(g->lib_dir) - 4) : 0, g->lib_dir);
      lib = realpath(root_dir, NULL);
    }
    snprintf(root_dir, sizeof root_dir, "%s", lib ? lib : g->lib_dir);
    free(lib);
    if (dirs) emit_str(root, "load_dirs", dirs);
    emit_str(root, "load_root", root_dir);
    free(dirs);
  }
  /* each site made one LoadNode, and each LoadNode took a site */
  int loads = 0, once = 1;
  for (int id = start; id < node_counter; id++) loads += nt_kind(out_nt, id) == NK_LoadNode;
  for (int i = 0; i < r->nsite; i++)
    loads += sp_pm_name_is(g_parser, ((const pm_call_node_t *)r->site[i].call)->name, "autoload");
  for (int i = 0; i < r->nsite; i++) once &= r->site[i].taken == 1;
  if (!once || loads != r->nsite) {
    fprintf(stderr, "spinel_parse: internal error: the requires of '%s' and its LoadNodes do not pair one to one\n",
            f ? g->file[f] : source_file);
    return 0;
  }
  return 1;
}

/* Boot library `b` loads at the program's start: its file a file of the
   graph, loaded by an edge of the entry's ahead of its statements (CRuby's
   core is loaded before any of them). Its edge; the LoadNode is made once
   the entry is flattened (sp_graph_boot_node). */
static int sp_graph_boot_load(SpLoadGraph *g, const SpBootLib *b) {
  sp_feature_mark(b->lib);
  sp_note_required_feature(b->lib);
  const char **bf = realloc(sp_boot_features, sizeof(char *) * (size_t)(sp_nboot_features + 1));
  if (bf) { sp_boot_features = bf; sp_boot_features[sp_nboot_features++] = strdup(b->lib); }
  int to = sp_load_file(g, b->path);
  if (b->tier == SP_LIB_BUILTIN) *sp_load_flags(g, to) |= SP_FILE_BUILTIN;
  sp_load_set_tier(g, to, b->tier);
  sp_load_edge(&g->edge, &g->nedge, &g->edgecap, 0, 0, 1, b->lib, SP_LOAD_FILE, to);
  g->edge[g->nedge - 1].top = 1;
  return g->nedge - 1;
}

/* Boot library edge `e`'s LoadNode, statement `at` of the entry's body: the
   boot libraries ahead of the entry's statements in the order they load,
   as their statement indices (`b - nboot`, sp_graph_boot) say. */
static void sp_graph_boot_node(SpLoadGraph *g, int e, int at) {
  int id = node_counter++;
  nt_put_type(out_nt, id, "LoadNode");
  emit_str(id, "method", "require");
  emit_str(id, "path", g->edge[e].name);
  emit_str(id, "how", "file");
  emit_int(id, "target_file", g->edge[e].to);
  emit_int(id, "boot", 1);
  g->edge[e].node = id;
  int st = nt_ref(out_nt, out_nt->root_id, "statements"), n = 0;
  const int *v = nt_arr(out_nt, st, "body", &n);
  int *nv = malloc(sizeof(int) * (size_t)(n + 1));
  if (!nv) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  if (at) memcpy(nv, v, sizeof(int) * (size_t)at);
  nv[at] = id;
  if (n > at) memcpy(nv + at + 1, v + at, sizeof(int) * (size_t)(n - at));
  nt_node_set_arr(out_nt, st, "body", nv, n + 1);
  free(nv);
}

/* Is edge `e` a require of `feature` the runtime answers natively? */
static int sp_edge_native_require(const SpLoadEdge *e, const char *feature) {
  size_t fl = strlen(feature);
  return e->how == SP_LOAD_NATIVE && strncmp(e->name, feature, fl) == 0 &&
         (!e->name[fl] || strcmp(e->name + fl, ".rb") == 0);
}

/* Add-on `b` loads with the require that makes it available: each
   `require "<rides>"` edge, the runtime's native one until now, resolves to
   the add-on's file instead, and its LoadNode takes that when its file is
   flattened. */
static void sp_graph_addon_load(SpLoadGraph *g, const SpBootLib *b) {
  int to = sp_load_file(g, b->path);
  sp_load_set_tier(g, to, b->tier);
  for (int i = 0; i < g->nedge; i++)
    if (sp_edge_native_require(&g->edge[i], b->rides)) { g->edge[i].how = SP_LOAD_FILE; g->edge[i].to = to; }
}

/* Does the program name what library `b` defines? `facts` are the names of
   every file read so far; `dyn` says its literals count as names too. */
static int sp_boot_lib_named(SpNames *facts, SpBootLib *b, int dyn) {
  for (int i = 0; i < b->entry.n; i++) {
    const char *e = b->entry.v[i];
    if (sp_names_has(facts, e)) return 1;
    if (dyn && e[0] != 'l') {
      char k[300];
      snprintf(k, sizeof k, "s:%s", e + 2);
      if (sp_names_has(facts, k)) return 1;
      if (e[0] == 'c') { snprintf(k, sizeof k, "m:%s", e + 2); if (sp_names_has(facts, k)) return 1; }
    }
  }
  return 0;
}

/* The libraries the program uses that CRuby has loaded before it starts
   (builtins/, and the packages whose manifests say `boot`), and the add-ons
   its requires make available (the packages whose manifests say `rides`):
   a library loads when the names the files read so far use meet the names
   it defines, until no more does. Each loaded library's files are read
   (sp_graph_read), so their names count for the rest. */
static int sp_graph_boot(SpLoadGraph *g, const char *source_file, SpNames *facts) {
  SpBootLib *libs = NULL;
  int nlib = 0, cap = 0;
  char dir[1024];
  int no_builtins = getenv("SPINEL_NO_BUILTINS") != NULL, have_enum = 0;
  /* builtins/: every file, in name order */
  if (!no_builtins && sp_builtins_dir(g->lib_dir, dir, sizeof dir)) {
    DIR *d = opendir(dir);
    struct dirent *de;
    char **names = NULL;
    int nn = 0, ncap = 0;
    while (d && (de = readdir(d)) != NULL) {
      size_t l = strlen(de->d_name);
      if (l < 4 || strcmp(de->d_name + l - 3, ".rb") != 0) continue;
      names = sp_grow_strs(names, nn, &ncap, 16);
      names[nn] = malloc(l + 7);
      if (!names[nn]) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      snprintf(names[nn], l + 7, "builtins/%.*s", (int)(l - 3), de->d_name);
      nn++;
    }
    if (d) closedir(d);
    qsort(names, (size_t)nn, sizeof *names, sp_names_cmp);
    for (int i = 0; i < nn; i++) {
      SpBootLib *b = sp_boot_lib_slot(&libs, nlib, &cap);
      if (sp_boot_lib_find(g, names[i], b) && sp_boot_lib_read(b)) {
        if (!strcmp(names[i], "builtins/enumerable")) have_enum = 1;
        nlib++;
      }
      else sp_names_free(&b->entry);
      free(names[i]);
    }
    free(names);
  }
  if (!no_builtins && !have_enum) {
    /* the core methods written in Ruby ship beside the compiler */
    fprintf(stderr, "spinel: builtins/enumerable.rb not found beside the compiler (looked under %s);\n"
                    "        the toolchain is incomplete -- it ships with `make install`. SPINEL_NO_BUILTINS=1 compiles without it.\n",
            g->lib_dir);
    for (int i = 0; i < nlib; i++) sp_names_free(&libs[i].entry);
    free(libs);
    return 0;
  }
  const PackageFact *pf;
  for (int i = 0; (pf = package_fact_at(i)) != NULL; i++) {
    if (!pf->boot) continue;
    SpBootLib *b = sp_boot_lib_slot(&libs, nlib, &cap);
    if (sp_boot_lib_find(g, pf->feature, b) && sp_boot_lib_read(b)) nlib++;
    else sp_names_free(&b->entry);
  }
  int ok = 1;
  for (int changed = 1; changed && ok; ) {
    changed = 0;
    /* an add-on is available once a require of what it rides is written,
       by the program or by a library loaded in the round before */
    for (int i = 0; (pf = package_fact_at(i)) != NULL; i++) {
      if (!pf->rides) continue;
      int offered = 0, required = 0;
      for (int k = 0; k < nlib && !offered; k++) offered = libs[k].rides == pf->rides;
      for (int e = 0; e < g->nedge && !offered && !required; e++) required = sp_edge_native_require(&g->edge[e], pf->rides);
      if (offered || !required) continue;
      SpBootLib *b = sp_boot_lib_slot(&libs, nlib, &cap);
      memset(b, 0, sizeof *b);
      snprintf(b->lib, sizeof b->lib, "%s", pf->feature);
      b->rides = pf->rides;
      b->tier = SP_LIB_PACKAGE;
      if (sp_package_file(g->lib_dir, pf->package, pf->feature, b->path, sizeof b->path) && sp_boot_lib_read(b)) nlib++;
      else sp_names_free(&b->entry);
    }
    int dyn = sp_names_has(facts, "dyn");
    for (int i = 0; i < nlib && ok; i++) {
      SpBootLib *b = &libs[i];
      if (b->loaded || !sp_boot_lib_named(facts, b, dyn)) continue;
      b->loaded = changed = 1;
      int from = g->nfile;
      if (b->rides) sp_graph_addon_load(g, b);
      else {
        if (g->nboot == g->bootcap) {
          g->bootcap = g->bootcap ? g->bootcap * 2 : 8;
          g->boot = realloc(g->boot, sizeof *g->boot * (size_t)g->bootcap);
          if (!g->boot) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
        }
        g->boot[g->nboot++] = sp_graph_boot_load(g, b);
      }
      for (int f = from; f < g->nfile && ok; f++) ok = sp_graph_read(g, f, source_file, facts);
    }
  }
  /* each boot library's statement ahead of the entry's first, in order */
  for (int b = 0; b < g->nboot; b++) g->edge[g->boot[b]].stmt = b - g->nboot;
  for (int i = 0; i < nlib; i++) sp_names_free(&libs[i].entry);
  free(libs);
  return ok;
}

/* ---- Load order ----
 *
 * When each file's top level runs, as CRuby runs it: the entry's statements
 * in order, the libraries CRuby has at boot ahead of them, and a file loaded
 * by a require that is itself a top-level statement of a file placed so,
 * run inside that statement the first time one reaches it. `entered[f]` is
 * that require's edge (-2 for the entry). Every other file is not placed:
 * a require under a condition, in a method, block or class body, or in a
 * value may run it at another time. (A file a computed path may name is
 * placed here as a literal require places it; the program module takes it
 * out of that place where the computed require may run first,
 * prog_settle_files.) */
static void sp_load_enter(SpLoadGraph *g, int e) {
  const SpLoadEdge *x = &g->edge[e];
  if (!x->top || x->how != SP_LOAD_FILE || g->entered[x->to] != -1) return;
  g->entered[x->to] = e;
  /* its own top-level requires, statement by statement (as its sites are) */
  const SpGraphFile *r = g->gf[x->to];
  for (int i = 0; i < r->nsite; i++) sp_load_enter(g, r->site[i].edge);
}
/* Does a boot library's load run file `f`'s top level (an edge of g->boot,
   or the top-level requires of a file one runs)? It runs before the entry's
   first statement, so no other require runs it first. */
static int sp_load_by_boot(const SpLoadGraph *g, int f) {
  while (g->entered[f] >= 0) {
    int e = g->entered[f];
    for (int b = 0; b < g->nboot; b++) if (g->boot[b] == e) return 1;
    f = g->edge[e].from;
  }
  return 0;
}
static int sp_load_path(const SpLoadGraph *g, int f, int s, int *fs, int *ss, int cap);
/* Does a require that is no statement of a top level, x, run after the
   top-level require that places its file? It stands in a top-level
   statement of a placed file, and nothing under that statement runs
   before the statement does (a method it defines is called, a block it
   makes runs, once it has; not a `BEGIN`, which runs ahead of the file's
   other statements): when that statement comes after the placing
   require's in the load order, the file is loaded by then, and x loads
   nothing. */
/* Does require x, standing in a top-level statement of a placed file,
   run only after the require `by` (an edge)? When x's statement comes
   after by's in the load order: nothing under a statement runs before the
   statement does. Or when x runs inside the file by loads, at any depth
   (`class Late; require_relative "msg"; end` in the file by runs): by has
   begun by then, and its file is in the loaded features. */
static int sp_load_edge_after(const SpLoadGraph *g, const SpLoadEdge *x, int by) {
  if (by < 0 || g->entered[x->from] == -1 || x->stmt < 0 || x->begin_block) return 0;
  const SpLoadEdge *e = &g->edge[by];
  enum { CAP = 64 };
  int xf[CAP], xs[CAP], ef[CAP], es[CAP];
  int nx = sp_load_path(g, x->from, x->stmt, xf, xs, CAP), ne = sp_load_path(g, e->from, e->stmt, ef, es, CAP);
  if (!nx || !ne || nx == CAP || ne == CAP) return 0;
  for (int i = 0; i < nx && i < ne; i++) {
    if (xf[i] != ef[i]) return 0;
    if (xs[i] != es[i]) return xs[i] > es[i];
  }
  return nx > ne && g->entered[xf[ne]] == by;
}
static int sp_load_nested_after(const SpLoadGraph *g, const SpLoadEdge *x) {
  return sp_load_edge_after(g, x, g->entered[x->to]);
}
/* Does unplaced file f run only after the require `by`? Each require that
   may run it stands in a top-level statement of a placed file after by's
   (sp_load_edge_after), or in an unplaced file that runs only after by in
   turn: a file `later.rb` that only a method defined after by's statement
   requires runs after it, and a require in it of by's file loads nothing.
   A requirer the walk is already asking about (a cycle of requires) adds
   no way in: the files of the cycle run only once one of them is entered
   from outside it, and those requires are asked. No for the entry, or past
   a depth of files. */
static int sp_load_file_after_in(const SpLoadGraph *g, int f, int by, int *asking, int depth) {
  enum { DEPTH = 32 };
  if (f <= 0 || depth >= DEPTH) return 0;
  asking[depth] = f;
  int any = 0;
  for (int e = 0; e < g->nedge; e++) {
    const SpLoadEdge *y = &g->edge[e];
    if (y->to != f) continue;
    if (y->how != SP_LOAD_FILE) return 0;
    any = 1;
    if (g->entered[y->from] != -1) {
      if (!sp_load_edge_after(g, y, by)) return 0;
      continue;
    }
    int cyc = 0;
    for (int k = 0; k <= depth && !cyc; k++) cyc = asking[k] == y->from;
    if (!cyc && !sp_load_file_after_in(g, y->from, by, asking, depth + 1)) return 0;
  }
  return any;
}
static int sp_load_file_after(const SpLoadGraph *g, int f, int by, int depth) {
  int asking[32];
  (void)depth;
  return sp_load_file_after_in(g, f, by, asking, 0);
}
static void sp_load_order_build(SpLoadGraph *g) {
  g->entered = malloc(sizeof(int) * (size_t)(g->nfile + 1));
  if (!g->entered) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int f = 0; f < g->nfile; f++) g->entered[f] = -1;
  g->entered[0] = -2;
  for (int b = 0; b < g->nboot; b++) sp_load_enter(g, g->boot[b]);
  for (int i = 0; i < g->gf[0]->nsite; i++) sp_load_enter(g, g->gf[0]->site[i].edge);
  /* a file another require may run first is not placed, nor is any file
     only such a file places (not a boot library's: nothing runs before it);
     a require in an unplaced file that runs only after the placing require
     loads nothing (sp_load_file_after) */
  for (int changed = 1; changed; ) {
    changed = 0;
    for (int e = 0; e < g->nedge; e++) {
      const SpLoadEdge *x = &g->edge[e];
      if (x->how != SP_LOAD_FILE || x->to <= 0 || g->entered[x->to] == -1 || sp_load_by_boot(g, x->to)) continue;
      int by = g->entered[x->to];
      int from_after = g->entered[x->from] == -1 && sp_load_file_after(g, x->from, by, 0);
      if ((!from_after && ((!x->top && !sp_load_nested_after(g, x)) || g->entered[x->from] == -1)) ||
          (by >= 0 && g->entered[g->edge[by].from] == -1)) {
        g->entered[x->to] = -1;
        changed = 1;
      }
    }
  }
}

/* The places of top-level statement `s` of file `f`, outermost first: each
   file's statement that runs it, from the entry's down to `s` itself. The
   count, 0 when the file is not placed. */
static int sp_load_path(const SpLoadGraph *g, int f, int s, int *fs, int *ss, int cap) {
  int n = 0;
  while (n < cap) {
    if (g->entered[f] == -1) return 0;
    fs[n] = f; ss[n] = s; n++;
    if (g->entered[f] == -2) break;
    const SpLoadEdge *x = &g->edge[g->entered[f]];
    f = x->from; s = x->stmt;
  }
  for (int i = 0; i < n / 2; i++) {
    int t = fs[i]; fs[i] = fs[n - 1 - i]; fs[n - 1 - i] = t;
    t = ss[i]; ss[i] = ss[n - 1 - i]; ss[n - 1 - i] = t;
  }
  return n;
}

/* --dump-load-graph: where each file's top level runs, as the statements
   that run it (file:statement, outermost first), or `unplaced`. */
static void sp_load_order_dump(const SpLoadGraph *g) {
  int n = g->nfile + 1;
  int *fs = malloc(sizeof(int) * (size_t)n * 2);
  if (!fs) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int f = 0; f < g->nfile; f++) {
    int k = sp_load_path(g, f, 0, fs, fs + n, n);
    printf("order %d:", f);
    if (!k) printf(" unplaced");
    for (int i = 0; i + 1 < k; i++) printf(" %d:%d", fs[i], fs[n + i]);
    printf("\n");
  }
  free(fs);
}

/* The names builtins/enumerable.rb and the scalar containers' files
   define, which the analysis rewrites calls onto (desugar_builtins,
   desugar_builtin_scalar_defs), read whether the program loads the files
   or not. */
static void sp_graph_builtin_names(const char *lib_dir) {
  if (getenv("SPINEL_NO_BUILTINS")) return;
  char found[1024];
  if (sp_builtin_enum_names_n == 0 &&
      sp_lib_lookup(lib_dir, "builtins/enumerable", found, sizeof found) == SP_LIB_BUILTIN) {
    char *text = read_file(found);
    if (text) { sp_builtin_names_from(text); free(text); }
  }
  for (int i = 0; i < SP_BUILTIN_EXTRA_N; i++) {
    SpBuiltinExtra *bf = &sp_builtin_extras[i];
    if (bf->scanned) continue;
    bf->scanned = 1;
    if (sp_lib_lookup(lib_dir, bf->req, found, sizeof found) != SP_LIB_BUILTIN) continue;
    char *text = read_file(found);
    if (text) { sp_builtin_names_from_into(text, &bf->names, &bf->names_n); free(text); }
  }
}

/* the load order handed to the analysis, as data in the
   node table: each placed file's root carries `load_by`, the LoadNode whose
   edge first runs its top level (entered[]); an unplaced file's root has
   none. The analysis orders two files' nodes from it (prog_order), at the
   LoadNodes' places in the table as they stand after any rewriting. */
static void sp_load_order_export(const SpLoadGraph *g) {
  for (int f = 1; f < g->nfile; f++) {
    int e = g->entered[f];
    if (e >= 0) emit_int(g->gf[f]->root, "load_by", g->edge[e].node);
  }
}

/* SPINEL_GRAPH_LAYOUT=reverse (a test hook, not a mode): lay the files'
   nodes out in the reverse of the graph's order, the entry's last, each
   file's own nodes in their order. Node ids say nothing about the order of
   two files (the analysis asks the program module, prog_order), so the
   program compiles to the same types, output and refusals either way (the
   C differs only in names and numbering a walk gives in id order); a
   difference names a reader that read ids as the order across files. */
static void sp_graph_layout_hook(NodeTable *nt) {
  const char *e = getenv("SPINEL_GRAPH_LAYOUT");
  if (!e || strcmp(e, "reverse") != 0) return;
  int n = nt->count, k = 0;
  int *file = malloc(sizeof(int) * (size_t)(n ? n : 1)), *order = malloc(sizeof(int) * (size_t)(n ? n : 1));
  if (!file || !order) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int id = 0; id < n; id++) file[id] = -1;
  for (int f = 0; f < nt->nfile_roots; f++) {   /* each file's tree, from its root */
    int sp = 0;
    order[sp++] = nt->file_roots[f];
    while (sp > 0) {
      int id = order[--sp];
      if (id < 0 || id >= n || file[id] >= 0) continue;
      file[id] = f;
      const SpNode *nd = &nt->nodes[id];
      for (int j = 0; j < nd->nr; j++) if (nd->r[j].ref >= 0) order[sp++] = nd->r[j].ref;
      for (int j = 0; j < nd->na; j++) for (int q = 0; q < nd->a[j].n; q++) order[sp++] = nd->a[j].ids[q];
    }
  }
  for (int f = nt->nfile_roots - 1; f >= -1; f--)
    for (int id = 0; id < n; id++) if (file[id] == f) order[k++] = id;
  nt_renumber(nt, order);
  free(file); free(order);
}

/* the program as the files it loads, the libraries
   CRuby has loaded before it starts among them (sp_graph_boot): every file
   read and parsed first (sp_graph_read), then each flattened as a root of
   its own (sp_graph_flatten), each file's root in the table's file_roots,
   and the load order exported onto the roots (sp_load_order_export). */
static NodeTable *sp_parse_graph(const char *source_file, const char *argv0) {
  SpLoadGraph g;
  memset(&g, 0, sizeof g);
  sp_lib_dir(argv0, g.lib_dir, sizeof g.lib_dir);
  sp_graph_builtin_names(g.lib_dir);
  out_nt = nt_new_table();
  node_counter = 0;
  sp_load_file(&g, source_file);
  g.niroots = sp_feature_roots_n;
  g.iroots = malloc(sizeof *g.iroots * (size_t)(g.niroots + 1));
  if (!g.iroots) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  for (int i = 0; i < g.niroots; i++)
    if (!(g.iroots[i] = strdup(sp_feature_roots[i]))) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  SpNames facts = { NULL, 0, 0, 0 };
  int ok = 1;
  /* every file of the program read first, the libraries CRuby has at boot
     among them, then each flattened: what a pass over the whole program
     asks of the trees is known before any file becomes nodes */
  for (int read_to = 0; ok; ) {
    while (read_to < g.nfile && ok) ok = sp_graph_read(&g, read_to++, source_file, &facts);
    if (!ok || !g.pre_tightened) break;
    g.pre_tightened = 0;
    for (int f = 0; f < read_to; f++)
      sp_graph_scan_sites(&g, f, f ? g.file[f] : source_file, &facts, &g.pre_inherit);
  }
  sp_roots_set(&g, NULL, 0);   /* (the libraries CRuby has at boot are found as the program starts) */
  if (ok) ok = sp_graph_boot(&g, source_file, &facts);
  if (ok && g.pre_tightened) {
    for (int read_to = g.nfile; ok; ) {
      g.pre_tightened = 0;
      for (int f = 0; f < read_to; f++)
        sp_graph_scan_sites(&g, f, f ? g.file[f] : source_file, &facts, &g.pre_inherit);
      while (read_to < g.nfile && ok) ok = sp_graph_read(&g, read_to++, source_file, &facts);
      if (!g.pre_tightened) break;
    }
  }
  for (int f = 0; f < g.nfile && ok; f++) {
    if (!(*sp_load_flags(&g, f) & SP_FILE_BUILTIN))
      sp_load_sites_note(&g.gf[f]->src.parser, g.gf[f]->src.root, 1, &g.pre_inherit);
  }
  sp_names_free(&facts);
  if (ok) sp_load_order_build(&g);
  /* inline RBS resolves a type name against every declaration of the
     program, as RBS does, whichever file declares it: each file's classes
     and modules are known before any file's annotations are read */
  int rbs = 0;
  for (int f = 0; f < g.nfile && ok && !rbs; f++)
    rbs = !(*sp_load_flags(&g, f) & SP_FILE_BUILTIN) && sp_inline_rbs_any(&g.gf[f]->src.parser);
  for (int f = 0; f < g.nfile && ok && rbs; f++)
    sp_inline_rbs_declare(&g.gf[f]->src.parser, g.gf[f]->src.root, g.gf[f]->text, strlen(g.gf[f]->text));
  for (int f = 0; f < g.nfile && ok; f++) ok = sp_graph_flatten(&g, f, source_file);
  for (int b = 0; b < g.nboot && ok; b++) sp_graph_boot_node(&g, g.boot[b], b);
  if (ok) sp_pre_emit_cli_defines(out_nt->root_id, g.nboot);
  if (ok) sp_load_order_export(&g);
  if (sp_req_written != sp_req_resolved) g_require_unread = 1;
  NodeTable *nt = out_nt;
  out_nt = NULL;
  if (!ok) {   /* a file that could not be read or parsed, reported above */
    nt_free(nt);
    nt = NULL;
    sp_parse_reported = 1;
  }
  else {
    nt_put_source_file(nt, source_file);
    for (int i = 0; i < sp_file_count; i++) nt_put_file(nt, i, sp_file_table[i]);
    sp_defer_paths_mark(nt);
    sp_file_tiers_mark(nt);
    nt->file_roots = calloc((size_t)g.nfile, sizeof(int));
    if (!nt->file_roots) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    nt->nfile_roots = g.nfile;
    for (int id = 0; id < nt->count; id++)
      if (nt_kind(nt, id) == NK_ProgramNode && nt_int(nt, id, "load_file", -1) >= 0)
        nt->file_roots[nt_int(nt, id, "load_file", 0)] = id;
    sp_graph_layout_hook(nt);
    if (g_dump_load_graph) { sp_load_graph_dump(&g); sp_load_order_dump(&g); }
  }
  sp_load_graph_free(&g);
  return nt;
}

int g_opt_debug, g_opt_line_map, g_opt_positions, g_opt_warn_widen, g_opt_check_stores,
    g_opt_defer_refusals, g_opt_emit_types_keep_c, g_opt_strict_rbs, g_opt_warn_verbose;
const char *g_opt_emit_types, *g_opt_emit_rbs, *g_opt_emit_symbol_map, *g_opt_profile_symbol_map,
           *g_opt_rbs_seed, *g_opt_eval_path;

/* Whether flatten stamps each node's position (node_line / node_file /
   node_col, and its end where a report reads it), as the debug and report
   modes ask; set before the requires are resolved, so the resolvers insert
   the PUSH/POP markers the multi-file source map is rebuilt from. */
static void sp_parse_positions_from_opts(void) {
  int on = g_opt_debug || g_opt_line_map || g_opt_positions;
  g_emit_line = on ? 1 : 0;
  g_emit_end = (on && (g_opt_emit_types || g_opt_warn_widen)) ? 1 : 0;
}

static NodeTable *sp_parse_emit(const char *source_file, const char *argv0) {
  sp_parse_positions_from_opts();
  /* `--strict-require` is the flag spelling of SPINEL_STRICT_REQUIRE. An env
     assignment cannot ride inside a flag string, and `spin flags` has to hand
     a caller's Makefile the same gate `spin build` compiles under. */
  g_strict_require = (g_strict_require_cli || getenv("SPINEL_STRICT_REQUIRE")) ? 1 : 0;
  return sp_parse_graph(source_file, argv0);
}

/* The library copy of the Prism front-end, linked into the C compiler
   (build/spinel): only the in-process lib API, no standalone CLI main. */

/* In-process entry for the single-binary C compiler: parse `source_file`
   into a node table (the caller frees it with nt_free), or NULL on error.
   `argv0` locates the stdlib for plain requires. */
NodeTable *sp_parse_file(const char *source_file, const char *argv0) {
  return sp_parse_emit(source_file, argv0);
}

/* Parse a Ruby snippet -- compiler-written text, or a class_eval string a
   desugar can read at compile time -- into a node table of its own, as the
   entry program is. NULL when it does not parse. The flattener's globals
   belong to the entry parse, which is over by the time this runs; they are
   saved around the call and reset the way that parse resets them. The
   entry's source map and per-line frozen_string_literal table describe the
   entry buffer, not this text, so they are off for it.
   With no frame the text is parsed as a file of its own: no locals around
   it, a node's line its line in the text (when the entry has lines at all),
   and NULL for text that does not read the same spliced into a body
   (sp_snippet_graftable). With a frame it is parsed as CRuby parses an
   eval's text: the frame's locals are Prism's scopes, so a name the frame
   holds reads that local; `__LINE__` counts from the frame's line and
   `__FILE__` answers its path, "(eval at FILE:LINE)" by default, FILE being
   what `__FILE__` answers at the site; the text's own source map gives
   each node the site's line and file, from its line in the text, as
   positions handed to `#line` and the reports. The table's source_file is
   the path. */
NodeTable *sp_parse_snippet(const char *src) {
  if (!src || !sp_snippet_graftable(src)) return NULL;
  return sp_parse_snippet_in(src, NULL);
}
NodeTable *sp_parse_snippet_in(const char *src, const SnipFrame *f) {
  if (!src || (f && f->line < 1)) return NULL;
  /* the entry buffer's state the flattener reads: its source map, per-line
     frozen_string_literal table, the statement cursor and owner, the file
     `__FILE__` answers, and whether the file flattened last is a builtins/
     one (sp_in_builtin), which the text is not */
  const pm_parser_t *saved_parser = g_parser;
  int saved_emit_line = g_emit_line, saved_emit_end = g_emit_end;
  NodeTable *saved_nt = out_nt; int saved_counter = node_counter;
  int saved_map_n = sp_line_map_n; unsigned char *saved_fsl = g_fsl_lines; size_t saved_fsl_n = g_fsl_nlines;
  int *saved_lf = sp_line_file, *saved_lo = sp_line_orig;
  int *saved_df = sp_disp_file, *saved_dl = sp_disp_line;
  const char *saved_src = g_source_file; int saved_flat = g_flat_file, saved_bi = g_flat_builtin;
  const uint8_t *saved_owner = g_owner;
  sp_line_map_n = 0; g_fsl_lines = NULL; g_fsl_nlines = 0;
  g_owner = NULL; g_emit_end = 0; g_flat_builtin = 0;
  char *buf = strdup(src), *path = NULL;
  int *map = NULL;
  pm_options_t opts;
  sp_pm_options_init(&opts, NULL);
  if (f && buf) {
    if (f->path) path = strdup(f->path);
    else {
      char *site = f->site_file > 0 && f->site_file < sp_file_count && sp_file_table[f->site_file]
                   ? sp_required_file_path(sp_file_table[f->site_file]) : strdup(g_source_file);
      size_t cap = strlen(site) + 48;
      path = malloc(cap);
      if (path) snprintf(path, cap, "(eval at %s:%lld)", site, f->site_line);
      free(site);
    }
    if (path) pm_options_filepath_set(&opts, path);
    /* the frame's scopes, then the text's own, which Prism parses the
       text's top level in: a local the frame holds reads at depth 1 or more */
    if (f->nscopes > 0 && pm_options_scopes_init(&opts, (size_t)f->nscopes + 1))
      for (int i = 0; i <= f->nscopes; i++) {
        int n = i < f->nscopes ? f->scopes[i].n : 0;
        pm_options_scope_t *sc = &opts.scopes[i];
        if (!pm_options_scope_init(sc, (size_t)n)) continue;
        for (int j = 0; j < n; j++)
          pm_string_constant_init(&sc->locals[j], f->scopes[i].names[j], strlen(f->scopes[i].names[j]));
      }
    /* text line i: the eval's line f->line + i - 1, in no file, shown at
       the site's line + i - 1 in the site's file */
    int nl = 1;
    for (const char *q = buf; *q; q++) nl += *q == '\n';
    map = calloc((size_t)(nl + 2) * 4, sizeof(int));
    if (map) {
      sp_line_file = map; sp_line_orig = map + nl + 2;
      sp_disp_file = map + 2 * (nl + 2); sp_disp_line = map + 3 * (nl + 2);
      for (int i = 1; i <= nl; i++) {
        sp_line_orig[i] = (int)(f->line + i - 1);
        if (f->site_line > 0) { sp_disp_line[i] = (int)(f->site_line + i - 1); sp_disp_file[i] = f->site_file; }
      }
      sp_line_map_n = nl;
    }
    g_source_file = path ? path : "";
    g_flat_file = 0;
  }
  NodeTable *res = NULL;
  if (buf) {
    pm_parser_t parser;
    pm_parser_init(&parser, (const uint8_t *)buf, strlen(buf), &opts);
    pm_node_t *root = pm_parse(&parser);
    if (parser.error_list.size == 0) {
      g_parser = &parser;
      out_nt = nt_new_table(); node_counter = 0;
      for (int i = 0; f && i < f->nscopes; i++) decl_push(-1);   /* the host's scopes: no decl */
      out_nt->root_id = flatten(root);
      if (path) nt_put_source_file(out_nt, path);
      res = out_nt;
    }
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(buf);
  }
  pm_options_free(&opts);
  free(path); free(map);
  out_nt = saved_nt; node_counter = saved_counter;
  g_parser = saved_parser; g_emit_line = saved_emit_line; g_emit_end = saved_emit_end;
  sp_line_map_n = saved_map_n; g_fsl_lines = saved_fsl; g_fsl_nlines = saved_fsl_n;
  sp_line_file = saved_lf; sp_line_orig = saved_lo;
  sp_disp_file = saved_df; sp_disp_line = saved_dl;
  g_source_file = saved_src; g_flat_file = saved_flat; g_flat_builtin = saved_bi;
  g_owner = saved_owner;
  return res;
}
