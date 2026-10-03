/*
 * spinel_parse.c - Prism AST Serializer (C version)
 *
 * Equivalent to spinel_parse.rb but links with libprism directly.
 * Parses Ruby source and outputs line-based text AST for spinel_codegen.
 *
 * Build: cc -O2 -I$(PRISM)/include spinel_parse.c -L$(PRISM)/build -lprism -o spinel_parse
 *
 * Output format:
 *   ROOT <id>
 *   N <id> <type>           - node declaration
 *   S <id> <field> <escaped> - string field
 *   I <id> <field> <integer> - integer field
 *   F <id> <field> <float>   - float field
 *   R <id> <field> <ref_id>  - reference (-1 for nil)
 *   A <id> <field> <ids>     - array of references
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>      /* readlink: resolve the executable for gem lookup */
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#include <prism.h>

/* ---- In-memory output buffer ----
   The final text AST is assembled into a growable byte buffer rather than a
   FILE*, so the in-process entry point needs no open_memstream (POSIX.1-2008,
   absent on Solaris/AIX/HP-UX/MinGW and pre-10.13 macOS) nor a temp-file
   round-trip. One code path on every platform. */
typedef struct { char *data; size_t len; size_t cap; } SpStrBuf;

static void sb_ensure(SpStrBuf *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) return;
  size_t nc = sb->cap ? sb->cap : 4096;
  while (nc < sb->len + extra + 1) {
    if (nc > (((size_t)-1) / 2)) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    nc *= 2;
  }
  char *nd = realloc(sb->data, nc);
  if (!nd) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sb->data = nd;
  sb->cap = nc;
}

static void sb_puts(SpStrBuf *sb, const char *s) {
  size_t n = strlen(s);
  sb_ensure(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_printf(SpStrBuf *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: vsnprintf failed\n");
    exit(1);
  }
  sb_ensure(sb, (size_t)needed);
  vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap2);
  va_end(ap2);
  sb->len += (size_t)needed;
}

/* ---- Output buffer ---- */
static char **lines;
static size_t line_count;
static size_t line_cap;
static int node_counter;

static void out_add(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: vsnprintf failed\n");
    exit(1);
  }
  char *buf = malloc((size_t)needed + 1);
  if (!buf) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: out of memory\n");
    exit(1);
  }
  vsnprintf(buf, (size_t)needed + 1, fmt, ap2);
  va_end(ap2);
  if (line_count >= line_cap) {
    if (line_cap > (((size_t)-1) - 256) / 2) {
      fprintf(stderr, "spinel_parse: out of memory\n");
      exit(1);
    }
    size_t new_cap = (line_cap * 2) + 256;
    char **new_lines = realloc(lines, sizeof(char *) * new_cap);
    if (!new_lines) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    lines = new_lines;
    line_cap = new_cap;
  }
  lines[line_count++] = buf;
}

/* ---- Name from constant pool ---- */
static const pm_parser_t *g_parser;
static int sym_proc_block_starts_at(size_t off);
static const char *g_source_file = "";
static char *g_source_file_escaped = NULL;  /* escape_str(g_source_file), set once at init */
/* Debug builds: when SPINEL_DEBUG=1, flatten() emits a per-node
   `node_line` field so codegen can place C `#line` directives. Off by
   default so the AST text format (and golden tests) are unchanged. */
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
   every node is text AST the ordinary compile has no use for (#4522). */
static int g_emit_end = 0;
/* Set from the ENTRY file's `# frozen_string_literal: true` magic comment.
   The pragma is per-file in Ruby: the require resolvers build g_fsl_lines
   (one flag byte per line of the final spliced buffer, from each spliced
   file's own head), and flatten() stamps an `fzl` field on string-literal
   nodes from it. This global is the fallback -- used when the per-line
   table is unavailable (syntax-sugar changed the line count) -- and the
   whole-program gate read by the analyzer's mutable-string-buffer pass. */
int g_frozen_string_literal = 0;
/* Files with NO frozen_string_literal pragma default to 1: Spinel declares
   fsl:true semantics unconditionally (there is no chilled mode; an explicit
   `false` pragma warns and is ignored -- see docs/limitations.md). */
int g_fsl_default = 1;
/* Per-line pragma flags for the final spliced buffer (0-based line index). */
static unsigned char *g_fsl_lines = NULL;
static size_t g_fsl_nlines = 0;

/* `# frozen_string_literal: true` on the first line (or the second when the
   first is a shebang); scanning stops at the first non-comment line. */
static int sp_scan_fsl_pragma(const char *src) {
  const char *p = src;
  for (int line = 0; line < 2 && p && *p; line++) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *q = p; while (*q == ' ' || *q == '\t') q++;
    if (*q != '#') break;                      /* code reached: stop scanning */
    if (line == 0 && q[1] == '!') { p = nl ? nl + 1 : NULL; continue; }  /* shebang */
    char linebuf[512];
    size_t rem = len - (size_t)(q - p);
    size_t cl = rem < sizeof(linebuf) - 1 ? rem : sizeof(linebuf) - 1;
    memcpy(linebuf, q, cl); linebuf[cl] = '\0';
    const char *m = strstr(linebuf, "frozen_string_literal:");
    if (m) {
      m += strlen("frozen_string_literal:");
      while (*m == ' ' || *m == '\t') m++;
      if (strncmp(m, "true", 4) == 0) {
        char c = m[4];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 1;
      }
      else {
        /* Spinel has no chilled mode: string literals are always frozen
           (the mutable-string machinery needs the whole-program guarantee;
           see docs/limitations.md). An explicit opt-out is ignored loudly
           rather than honored partially. */
        fprintf(stderr,
                "warning: frozen_string_literal: false is not supported in "
                "Spinel (string literals are always frozen); the pragma is "
                "ignored\n");
      }
      return 1;
    }
    p = nl ? nl + 1 : NULL;
  }
  return g_fsl_default;
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

#include "sp_macro.c"

/* ---- String escaping ---- */
static char *escape_str(const uint8_t *src, size_t len) {
  /* Worst case: every char becomes %XX = 3x */
  char *out = malloc((len * 3) + 1);
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c == '%')       { out[j++]='%'; out[j++]='2'; out[j++]='5'; }
    else if (c == '\n') { out[j++]='%'; out[j++]='0'; out[j++]='A'; }
    else if (c == '\r') { out[j++]='%'; out[j++]='0'; out[j++]='D'; }
    else if (c == '\t') { out[j++]='%'; out[j++]='0'; out[j++]='9'; }
    else if (c == ' ')  { out[j++]='%'; out[j++]='2'; out[j++]='0'; }
    /* Issue #722: NUL byte inside a string literal would truncate
       the field at the AST text-serialization layer (lines split
       on '\n' and the loader uses strlen on fields). Encode as %00
       so the byte survives the round-trip. */
    else if (c == 0)    { out[j++]='%'; out[j++]='0'; out[j++]='0'; }
    else out[j++] = c;
  }
  out[j] = '\0';
  return out;
}

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

static char *escape_pm_string(const pm_string_t *s) {
  return escape_str(pm_string_source(s), pm_string_length(s));
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
static int sp_in_builtin(const uint8_t *at);   /* a builtins/ splice (below) */

/* ---- Emit helpers ---- */
static void emit_str(int id, const char *field, const char *val) {
  out_add("S %d %s %s", id, field, val);
}

static void emit_int(int id, const char *field, long long val) {
  out_add("I %d %s %lld", id, field, val);
}

static void emit_float(int id, const char *field, double val) {
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
  if (isinf(val)) { out_add("F %d %s %s", id, field, val > 0 ? "INFINITY" : "-INFINITY"); return; }
  if (isnan(val)) { out_add("F %d %s %s", id, field, "NAN"); return; }
  snprintf(buf, sizeof(buf), "%.17g", val);
  for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t l = strlen(buf);
    if (l + 3 < sizeof(buf)) strcat(buf, ".0");
  }
  out_add("F %d %s %s", id, field, buf);
}

static void emit_ref(int id, const char *field, pm_node_t *child) {
  int cid = child ? flatten(child) : -1;
  out_add("R %d %s %d", id, field, cid);
}

static void emit_node_array(int id, const char *field, pm_node_list_t *list) {
  if (!list || list->size == 0) {
    out_add("A %d %s ", id, field);
    return;
  }
  int *ids = malloc(sizeof(int) * list->size);
  for (size_t i = 0; i < list->size; i++)
    ids[i] = flatten(list->nodes[i]);
  /* Issue #744: build comma-separated string into a growable buffer.
     Previously a fixed 65536-byte stack buffer overflowed on
     20k+-element arrays -- snprintf returns the bytes it WOULD have
     written, advancing pos past the end and corrupting the stack. */
  size_t cap = 1024;
  char *buf = malloc(cap);
  size_t pos = 0;
  for (size_t i = 0; i < list->size; i++) {
    /* worst case per iter: ", -2147483648\0" -> 14 bytes; reserve 16 to be safe */
    if (pos + 16 >= cap) {
      size_t new_cap = (cap * 2) + 16;
      char *nb = realloc(buf, new_cap);
      if (!nb) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      buf = nb;
      cap = new_cap;
    }
    if (i > 0) buf[pos++] = ',';
    int n = snprintf(buf + pos, cap - pos, "%d", ids[i]);
    if (n > 0) pos += (size_t)n;
  }
  buf[pos] = '\0';
  out_add("A %d %s %s", id, field, buf);
  free(buf);
  free(ids);
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
    if (overflow || val >= max_negative) return -(long long)max_negative;
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
  if (integer->negative) return overflow || val >= max_negative;
  return overflow || val > max_positive;
}

/* frozen_string_literal flag for the file `node`'s source line came from:
   the per-line table when available, else the entry file's pragma. */
static int sp_node_fsl(pm_node_t *node) {
  if (g_fsl_lines) {
    int32_t bl = pm_newline_list_line(&g_parser->newline_list,
                                      node->location.start, g_parser->start_line);
    if (bl >= 1 && (size_t)bl <= g_fsl_nlines) return g_fsl_lines[bl - 1];
  }
  return g_frozen_string_literal;
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

/* ---- Main flattening ---- */
/* The builtin node the names invented from the node being flattened count
   from (`node_bi`, comp_node_ord): its enclosing def, or, outside every def,
   its top-level statement. -1 outside builtins/. */
static int g_bi_base = -1;
static int flatten_node(pm_node_t *node);
static int flatten(pm_node_t *node) {
  int saved = g_bi_base;
  int id = flatten_node(node);
  g_bi_base = saved;
  return id;
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
      if (sp_line_map_n > 0 && el >= 1 && el <= sp_line_map_n && sp_line_orig[el] > 0) eorig = sp_line_orig[el];
      emit_int(id, "node_end_line", (long long)eorig);
      emit_int(id, "node_end_col", (long long)le.column);
    }
  }

#define N(type_name) out_add("N %d " type_name, id)
#define S(field, val) do { char *_e = (val); emit_str(id, field, _e); free(_e); } while(0)
#define I(field, val) emit_int(id, field, val)
#define F(field, val) emit_float(id, field, val)
#define R(field, child) emit_ref(id, field, (pm_node_t *)(child))
#define A(field, list) emit_node_array(id, field, list)
#define NAME(field, cid) do { char *_n = cstr(cid); char *_e = escape_str((const uint8_t *)_n, strlen(_n)); emit_str(id, field, _e); free(_e); free(_n); } while(0)

  switch (t) {
  case PM_PROGRAM_NODE: {
    pm_program_node_t *n = (pm_program_node_t *)node;
    N("ProgramNode");
    R("statements", n->statements);
    break;
  }
  case PM_STATEMENTS_NODE: {
    pm_statements_node_t *n = (pm_statements_node_t *)node;
    pm_node_t **next = g_stmt_next, **end = g_stmt_end;
    N("StatementsNode");
    g_stmt_next = n->body.nodes; g_stmt_end = g_stmt_next + n->body.size;
    A("body", &n->body);
    g_stmt_next = next; g_stmt_end = end;
    break;
  }
  case PM_CLASS_NODE: {
    pm_class_node_t *n = (pm_class_node_t *)node;
    N("ClassNode");
    R("constant_path", n->constant_path);
    R("superclass", n->superclass);
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
          char *esc = escape_str((const uint8_t *)ap, strlen(ap));
          emit_str(id, "warn_path", esc);
          free(esc); free(ap);
        }
        else emit_str(id, "warn_path", g_source_file_escaped);
      }
      free(wn);
    }
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("block", n->block);
    /* Issue #793: emit explicit call_operator for non-safe-nav calls
       too, so codegen reads "." instead of an unset field. */
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    else {
      S("call_operator", escape_str((const uint8_t *)".", 1));
    }
    /* A bare identifier (no receiver/parens/args) is a variable-or-method
       read: an unresolved one is CRuby's NameError, not NoMethodError. */
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_VARIABLE_CALL)) I("vcall", 1);
    /* `__dir__` in a required file is that file's directory: carry the file
       for the compile-time folds, which otherwise see only the entry script */
    if (!n->receiver && !n->arguments) {
      char *cn = cstr(n->name);
      const char *rf = strcmp(cn, "__dir__") == 0 ? sp_node_required_file(node) : NULL;
      if (rf) {
        char *ap = sp_required_file_path(rf);
        char *esc = escape_str((const uint8_t *)ap, strlen(ap));
        emit_str(id, "src_file", esc);
        free(esc); free(ap);
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
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_AND_WRITE_NODE: {
    pm_index_and_write_node_t *n = (pm_index_and_write_node_t *)node;
    N("IndexAndWriteNode");
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_OR_WRITE_NODE: {
    pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)node;
    N("IndexOrWriteNode");
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_TARGET_NODE: {
    pm_index_target_node_t *n = (pm_index_target_node_t *)node;
    N("IndexTargetNode");
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
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_CALL_AND_WRITE_NODE: {
    pm_call_and_write_node_t *n = (pm_call_and_write_node_t *)node;
    N("CallAndWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_CALL_OR_WRITE_NODE: {
    pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)node;
    N("CallOrWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
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
    if (pm_int_overflows(&n->value)) {
      pm_buffer_t _bb;
      pm_buffer_init(&_bb);
      pm_integer_string(&_bb, &n->value);
      /* pm_buffer_value is not guaranteed NUL-terminated; bound by length. */
      size_t _bl = pm_buffer_length(&_bb);
      char *_bs = malloc(_bl + 1);
      memcpy(_bs, pm_buffer_value(&_bb), _bl);
      _bs[_bl] = '\0';
      emit_str(id, "bigval", _bs);
      free(_bs);
      pm_buffer_free(&_bb);
    }
    break;
  }
  case PM_FLOAT_NODE: {
    pm_float_node_t *n = (pm_float_node_t *)node;
    N("FloatNode");
    F("value", n->value);
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
       reduced pm_integer_t. */
    pm_rational_node_t *n = (pm_rational_node_t *)node;
    N("RationalNode");
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%lld", (long long)pm_int_value(&n->numerator));
    emit_str(id, "rat_num", nbuf);
    snprintf(nbuf, sizeof(nbuf), "%lld", (long long)pm_int_value(&n->denominator));
    emit_str(id, "rat_den", nbuf);
    break;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *n = (pm_string_node_t *)node;
    N("StringNode");
    S("content", escape_pm_string(&n->unescaped));
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
    S("value", escape_pm_string(&n->unescaped));
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
    /* PM_RANGE_FLAGS_EXCLUDE_END = 4. Codegen reads bit 2 to decide
       whether `..` (inclusive) or `...` (exclusive). */
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
    /* PM_LOOP_FLAGS_BEGIN_MODIFIER = 4 (bit 2): begin..end while form,
       which is a post-test loop (body runs at least once). The codegen
       reads bit 2 to decide between `while (cond) {}` and `do {} while (cond);`. */
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
    if (sym_proc_block_starts_at((size_t)(node->location.start - g_parser->start)))
      I("sym_proc_block", 1);
    /* Block-local variables (params + first-assigned-inside): Ruby scoping
       makes non-param locals FRESH on every block invocation; codegen needs
       the list to reset them per iteration in fused loops. */
    if (n->locals.size > 0) {
      /* join the names in one pass (single cstr per name, no strcat rescans) */
      size_t total = 1;
      char **nms = malloc(n->locals.size * sizeof(char *));
      if (nms) {
        for (size_t li = 0; li < n->locals.size; li++) {
          nms[li] = cstr(n->locals.ids[li]);
          total += strlen(nms[li]) + 1;
        }
        char *joined = malloc(total);
        if (joined) {
          char *w = joined;
          for (size_t li = 0; li < n->locals.size; li++) {
            if (li) *w++ = ',';
            size_t nl2 = strlen(nms[li]);
            memcpy(w, nms[li], nl2); w += nl2;
          }
          *w = '\0';
          out_add("S %d locals %s", id, joined);
          free(joined);
        }
        for (size_t li = 0; li < n->locals.size; li++) free(nms[li]);
        free(nms);
      }
    }
    /* Serialize block parameters */
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        int bpid = node_counter++;
        out_add("N %d BlockParametersNode", bpid);
        if (in_bi) emit_int(bpid, "node_bi", g_bi_base + 1);
        if (bp->parameters) {
          emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        }
        out_add("R %d %s %d", id, "parameters", bpid);
      }
else if (PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
        pm_numbered_parameters_node_t *np = (pm_numbered_parameters_node_t *)n->parameters;
        int npid = node_counter++;
        out_add("N %d NumberedParametersNode", npid);
        if (in_bi) emit_int(npid, "node_bi", g_bi_base + 1);
        emit_int(npid, "maximum", np->maximum);
        out_add("R %d %s %d", id, "parameters", npid);
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
  case PM_REQUIRED_PARAMETER_NODE: {
    pm_required_parameter_node_t *n = (pm_required_parameter_node_t *)node;
    N("RequiredParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_PARAMETER_NODE: {
    pm_optional_parameter_node_t *n = (pm_optional_parameter_node_t *)node;
    N("OptionalParameterNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_REST_PARAMETER_NODE: {
    pm_rest_parameter_node_t *n = (pm_rest_parameter_node_t *)node;
    N("RestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_PARAMETER_NODE: {
    pm_block_parameter_node_t *n = (pm_block_parameter_node_t *)node;
    N("BlockParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_ARGUMENT_NODE: {
    /* `&expr` in call argument position. Wraps the expression that
     * provides the proc to forward (typically a LocalVariableReadNode
     * for a `&block`-captured param, or a SymbolNode for `&:to_s`). */
    pm_block_argument_node_t *n = (pm_block_argument_node_t *)node;
    N("BlockArgumentNode");
    R("expression", n->expression);
    break;
  }
  case PM_BLOCK_LOCAL_VARIABLE_NODE: {
    pm_block_local_variable_node_t *n = (pm_block_local_variable_node_t *)node;
    N("BlockLocalVariableNode");
    NAME("name", n->name);
    break;
  }
  case PM_KEYWORD_REST_PARAMETER_NODE: {
    pm_keyword_rest_parameter_node_t *n = (pm_keyword_rest_parameter_node_t *)node;
    N("KeywordRestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_REQUIRED_KEYWORD_PARAMETER_NODE: {
    pm_required_keyword_parameter_node_t *n = (pm_required_keyword_parameter_node_t *)node;
    N("RequiredKeywordParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: {
    pm_optional_keyword_parameter_node_t *n = (pm_optional_keyword_parameter_node_t *)node;
    N("OptionalKeywordParameterNode");
    NAME("name", n->name);
    R("value", n->value);
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
    /* `__FILE__`. The entry script answers the path spinel was given
       (escaped once, in g_source_file_escaped); a required file answers
       its own absolute path, which the source map attributes the node to
       (#4839). */
    N("SourceFileNode");
    { const char *rf = sp_node_required_file(node);
      if (rf) {
        char *ap = sp_required_file_path(rf);
        char *esc = escape_str((const uint8_t *)ap, strlen(ap));
        emit_str(id, "content", esc);
        free(esc); free(ap);
      }
      else emit_str(id, "content", g_source_file_escaped); }
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
    S("content", escape_pm_string(&n->unescaped));
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
    S("unescaped", escape_pm_string(&n->unescaped));
    /* Emit Prism's regex flags so the codegen can pass /i, /x, /m
       through to the engine. PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE=4,
       _EXTENDED=8, _MULTI_LINE=16. */
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
    S("name", escape_str((const uint8_t *)"_1", 2));
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
    out_add("N %d StatementsNode", stmts_id);
    if (in_bi) emit_int(stmts_id, "node_bi", g_bi_base + 1);
    out_add("A %d body %d", stmts_id, var_id);
    out_add("R %d statements %d", id, stmts_id);
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
    char *kind_e = escape_str((const uint8_t *)pretty, plen);
    emit_str(id, "kind", kind_e);
    free(kind_e);
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
static int source_references_set(const char *src) {
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
            if (strncmp(r, "Set", 3) == 0 && sp_set_word_at(src, r)) return 1;
          for (const char *r = p + 2; r + 7 <= q; r++)
            if (strncmp(r, ".to_set", 7) == 0) return 1;
          p = q;
          continue;
        }
        p++;
      }
      if (*p) p++;
      continue;
    }
    if (strncmp(p, "Set", 3) == 0 && sp_set_word_at(src, p)) return 1;
    if (strncmp(p, ".to_set", 7) == 0) return 1;
    if (*p == '\n') bol = 1;
    p++;
  }
  return 0;
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
static int source_references_io_buffer(const char *src) {
  const char *p = src;
  int bol = 1;
  while (*p) {
    if (bol && strncmp(p, "=begin", 6) == 0) {
      const char *e = strstr(p, "\n=end");
      if (!e) return 0;
      p = e + 5;
      continue;
    }
    bol = 0;
    if (*p == '#') {
      while (*p && *p != '\n') p++;
      continue;
    }
    if (*p == '\'') {
      p++;
      while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
      if (*p) p++;
      continue;
    }
    if (*p == '"') {
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (p[0] == '#' && p[1] == '{') {
          const char *q = p + 2;
          int depth = 1;
          while (*q && depth) { if (*q == '{') depth++; else if (*q == '}') depth--; q++; }
          for (const char *r = p + 2; r < q; r++)
            if (strncmp(r, "IO::Buffer", 10) == 0 && sp_iob_word_at(src, r)) return 1;
          p = q;
          continue;
        }
        p++;
      }
      if (*p) p++;
      continue;
    }
    if (strncmp(p, "IO::Buffer", 10) == 0 && sp_iob_word_at(src, p)) return 1;
    if (*p == '\n') bol = 1;
    p++;
  }
  return 0;
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

/* The byte ranges of the final buffer a builtins/ file was spliced into.
   A node inside one is stamped `node_bi`, so the names the compiler invents
   from nodes number a builtin's nodes apart from the program's, and each
   builtin method's apart from the others': a builtin that grows or shrinks
   then moves only its own names (comp_node_ord).
   Read off the splice markers, which every build keeps, not off the line
   map, which only a build with line maps has. */
static size_t *sp_bi_lo = NULL, *sp_bi_hi = NULL;
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
      int bi = 0;
      for (size_t k = pl; k + 10 <= len && !bi; k++) if (!strncmp(line + k, "/builtins/", 10)) bi = 1;
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

  int *stk_file = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_next = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_start = (int *)malloc(sizeof(int) * (nlines + 2));
  if (!sp_line_file || !sp_line_orig || !sp_line_pop || !stk_file || !stk_next || !stk_start) {
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
    }
    bl++;
    if (!eol) break;
    line = eol + 1;
    if (*line == '\0') break;
  }
  free(stk_file);
  free(stk_next);
  free(stk_start);
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
      if (!is_cont) return ls;
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
   sits in, which the loaded file usually reopens. A receiver form
   (`Mod.autoload`) or a computed path is left as it was. */
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
      if (*q == ':') {
        q++;
        const char *ns = q;
        while (sp_req_ident_char(*q)) q++;
        if (q > ns) {
          while (*q == ' ' || *q == '\t') q++;
          if (*q == ',') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"' || *q == '\'') {
              char qq = *q++;
              const char *fs = q;
              while (*q && *q != qq && *q != '\n' && *q != '#' ) q++;
              if (*q == qq) {
                size_t flen = (size_t)(q - fs);
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (!paren || *q == ')') {
                  if (paren) q++;
                  const char *kw = sp_autoload_beside(dir, fs, flen) ? "require_relative" : "require";
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
    } else {
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
    if (!call->receiver && !call->block && call->arguments && call->arguments->arguments.size == 1 &&
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
    /* The `$LOAD_PATH << dir` that goes with a dead branch's require: the
       load-path pass below would otherwise warn about a statement that
       never runs, the way it does for a live one. */
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

static char *sp_rewrite_dead_requires(const char *source) {
  char *result = strdup(source);
  if (!strstr(source, "require")) return result;
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, strlen(source), NULL);
  pm_node_t *root = pm_parse(&parser);
  if (parser.error_list.size == 0) {
    SpDeadRequire ctx = { &parser, source, result, 0, !sp_source_writes_engine(source) };
    pm_visit_node(root, sp_skip_dead_require, &ctx);
  }
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  return result;
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

  /* computed requires spelling a literal path (#5700), then autoloads with
     a literal path (#5696): each answers a fresh copy of what it reads */
  char *reachable = sp_rewrite_dead_requires(source);
  char *pre_auto = sp_rewrite_computed_requires(reachable, dir);
  free(reachable);
  char *result = sp_rewrite_autoloads(pre_auto, dir);
  free(pre_auto);
  sp_autoload_is_main = 0;
  /* One pragma flag per line of `result`, kept in lockstep with every text
     splice below so each literal keeps its own file's flag. */
  size_t fsl_n;
  unsigned char *fsl = sp_fsl_make(result, sp_scan_fsl_pragma(source), &fsl_n);
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
    char *content;
    unsigned char *cfsl = NULL; size_t cfsl_n = 0;   /* spliced content's flags */
    if (sp_path_already_included(canonical)) {
      /* Already inlined once -- replace require with empty content */
      content = strdup("# require_relative skipped (already included)");
      cfsl = sp_fsl_make(content, 0, &cfsl_n);
      free(canonical);
      req_val = "false";
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
      if ((!hit.margin && !sp_req_in_control_only(result, hit.kw)) || (has_modifier && !ml)) {
        sp_req_hoist_splice(&result, &fsl, &fsl_n, &hit, content, cfsl, cfsl_n, req_val);
        free(cfsl);
        free(content);
        continue;
      }
    }

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
     silently for the same reason (#3455). */
  static const char *const tolerated[] = { "thread", "enumerator", "fiber", "ffi",
                                           "rational", "complex", NULL };
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
  int brk, fin, evals;
} SpSrcLex;
static SpSrcLex sp_src_lex;

static int sp_src_tok_is(const pm_token_t *tok, const char *word) {
  size_t wl = strlen(word);
  return (size_t)(tok->end - tok->start) == wl && memcmp(tok->start, word, wl) == 0;
}

static void sp_src_lex_token(void *data, pm_parser_t *parser, pm_token_t *tok) {
  SpSrcLex *lx = (SpSrcLex *)data;
  (void)parser;
  if (tok->type == PM_TOKEN_KEYWORD_BREAK) lx->brk = 1;
  if (tok->type != PM_TOKEN_IDENTIFIER) return;
  if (sp_src_tok_is(tok, "define_finalizer")) lx->fin = 1;
  if (sp_src_tok_is(tok, "eval") || sp_src_tok_is(tok, "class_eval") ||
      sp_src_tok_is(tok, "module_eval") || sp_src_tok_is(tok, "instance_eval")) lx->evals = 1;
}

/* The text is about to be freed or replaced: what was read from it goes. */
static void sp_src_lex_drop(void) {
  free(sp_src_lex.com);
  memset(&sp_src_lex, 0, sizeof sp_src_lex);
}

static SpSrcLex *sp_src_lex_of(const char *source) {
  SpSrcLex *lx = &sp_src_lex;
  if (lx->src == source) return lx;
  sp_src_lex_drop();
  lx->src = source;
  pm_parser_t parser;
  pm_lex_callback_t cb = { lx, sp_src_lex_token };
  pm_parser_init(&parser, (const uint8_t *)source, strlen(source), NULL);
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
    int word_start = !((bc >= 'a' && bc <= 'z') || (bc >= 'A' && bc <= 'Z') || (bc >= '0' && bc <= '9') || bc == '_' || bc == '@' || bc == '$' || bc == ':');
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
    if (bc == '.') return 1;
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

static int sp_src_defines_module(const char *source, const char *cname) {
  char def1[80], def2[80];
  snprintf(def1, sizeof def1, "module %s\n", cname);
  snprintf(def2, sizeof def2, "module %s ", cname);
  return strstr(source, def1) || strstr(source, def2);
}

static char *sp_prepend_require(char *source, const char *exe_path, const char *head,
                                unsigned char **fsl, size_t *fsl_n) {
  size_t sl = strlen(source), hl = strlen(head);
  char *ns = (char *)malloc(sl + hl + 1);
  if (!ns) return source;
  memcpy(ns, head, hl); memcpy(ns + hl, source, sl + 1);
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
  if (sp_src_opens(source, "module Enumerable")) return source;   /* the program reopens it itself: leave that alone for now */
  int any = 0;
  for (int i = 0; i < sp_builtin_enum_names_n && !any; i++)
    if (sp_source_mentions_method(source, sp_builtin_enum_names[i])) any = 1;
  /* the spellings the analyzer rewrites onto a builtin's name */
  static const char *const aliases[][2] = { { "with_object", "each_with_object" }, { "collect_concat", "flat_map" }, { "detect", "find" }, { NULL, NULL } };
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
  if (!strstr(source, "break")) return source;
  static const char *const names[] = {
    "map", "collect", "select", "filter", "reject", "filter_map", "each_with_object", "with_object",
    "inject", "reduce", "each_slice", "each_cons", "each_entry", "with_index", NULL
  };
  int any = 0;
  for (int i = 0; names[i] && !any; i++) if (sp_source_mentions_method(source, names[i])) any = 1;
  if (!any) return source;
  /* the keyword, not the word in a string or a comment */
  { SpSrcLex *lx = sp_src_lex_of(source); if (!lx->brk && !lx->evals) return source; }
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
const char *sp_builtin_extra_name(int idx, int i) {
  if (idx < 0 || idx >= SP_BUILTIN_EXTRA_N || i < 0 || i >= sp_builtin_extras[idx].names_n) return NULL;
  return sp_builtin_extras[idx].names[i];
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
    char *content;
    unsigned char *cfsl = NULL; size_t cfsl_n = 0;   /* spliced content's flags */
    if (sp_path_already_included(canonical)) {
      content = strdup("# require skipped (already included)");
      free(canonical);
      req_val = "false";
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
        /* pre-installed packages (the carved-out stdlib): packages/ sits
           beside lib/ in both the repo and the installed tree. The package
           root is the require root, so `require "erb"` is
           packages/erb/erb.rb and `require "erb/util"` is
           packages/erb/erb/util.rb. */
        char gp[1024];
        char first[256];
        {
          const char *sl = strchr(lib_name, '/');
          size_t fl2 = sl ? (size_t)(sl - lib_name) : strlen(lib_name);
          if (fl2 >= sizeof(first)) fl2 = sizeof(first) - 1;
          memcpy(first, lib_name, fl2);
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
        }
        if (!content) snprintf(gp, sizeof(gp), "%.*s/packages/%s/%s.rb", base_len, lib_dir, first, lib_name);
        if (!content) content = read_file(gp);
        if (!content) {
          /* dev binary one level below the repo root (build/spinel) */
          snprintf(gp, sizeof(gp), "%.*s/../packages/%s/%s.rb", base_len, lib_dir, first, lib_name);
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
        if (content) snprintf(lib_path, sizeof(lib_path), "%s", gp);
      }
      if (!content) {
        /* `-I <dir>` feature roots: <root>/X.rb, else <root>/X/<last>.rb. */
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
        if (root_dup) { free(content); content = strdup("# require skipped (already included)"); }
        else if (rc) sp_mark_path_included(rc);
        free(rc);
      }
      if (!content) {
        if (sp_lib_is_native(lib_name)) {
          /* Provided natively by the Spinel runtime; the require is a
             harmless no-op, so don't warn. The require-gate still records it
             so the C-native feature (e.g. io/console) becomes available. */
          sp_feature_mark(lib_name);
          content = strdup("# require provided by Spinel runtime");
          req_val = "false";   /* already there: nothing was loaded */
        }
else if (sp_require_tolerated(lib_name)) {
          /* A core capability Spinel provides without a file (Thread,
             Enumerator, Fiber, the ffi DSL); the require is a harmless
             no-op, like modern CRuby -- gate or no gate. */
          content = strdup("# require no-op (core feature)");
          req_val = "false";   /* already there: nothing was loaded */
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
        req_val = root_dup ? "false" : "true";   /* this require is what loaded it */
        char *resolved = resolve_requires(content, lib_path, &cfsl, &cfsl_n);
        free(content);
        content = resolved;
      }
    }
    /* Stub arms above leave cfsl NULL: their content is a one-line comment. */
    if (!cfsl) cfsl = sp_fsl_make(content, 0, &cfsl_n);
    if (sp_require_preloaded(lib_name)) req_val = "false";

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
      if ((!hit.margin && !sp_req_in_control_only(result, hit.kw)) || (has_modifier && !ml)) {
        sp_req_hoist_splice(&result, fsl, fsl_n, &hit, content, cfsl, cfsl_n, req_val);
        free(cfsl);
        free(content);
        continue;
      }
    }

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

/* ---- Syntax sugar rewriting ---- */

/* Ruby method-name char class: idents, digits, `?` / `!` / `=` suffixes,
   operator-method chars (`+`, `-`, `*`, etc.), and `[` / `]` for the
   index operators `[]` and `[]=`. Digits are allowed in the body but
   the macro rejects digit-leading names (invalid Ruby method syntax). */
static int sp_is_method_name_char(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '?' || c == '!' || c == '+' ||
         c == '-' || c == '*' || c == '/' || c == '<' || c == '>' ||
         c == '=' || c == '&' || c == '|' || c == '^' || c == '~' ||
         c == '%' || c == '[' || c == ']';
}

/* The names of the constant NAME that the `def_delegators ... *NAME` at
   `before` splats, read from the source text. Its one definition in the
   file must be a line of its own at the call's (indented) indentation, in
   the same class body with no heredoc between, holding a literal Symbol
   Array (`[:a, :b]` over any lines, or `%i[a b]`) followed by nothing but
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
  if (strncmp(src + q, "%i[", 3) == 0 || strncmp(src + q, "%i(", 3) == 0) {
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
    if (!pct) { if (src[q] != ':') { free(names); return 0; } q++; }
    /* a name, with a trailing ? ! or = (closed?, write!, v=), or :<< --
       the one operator the rewrite is known to delegate right */
    size_t s0 = q;
    if (sp_ident_char(src[q])) {
      while (q < before && sp_ident_char(src[q])) q++;
      if (src[q] == '?' || src[q] == '!' || (src[q] == '=' && src[q + 1] != '>')) q++;
    }
    else if (src[q] == '<' && src[q + 1] == '<' && src[q + 2] != '<' && src[q + 2] != '=') q += 2;
    if (q == s0 || q - s0 >= 160) { free(names); return 0; }
    if (n == cap) {
      cap *= 2;
      char (*nn)[160] = realloc(names, sizeof(*names) * (size_t)cap);
      if (!nn) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      names = nn;
    }
    memcpy(names[n], src + s0, q - s0);
    names[n][q - s0] = 0;
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

/* Where rewrite_syntax_sugar wrote the `{` / `do` of each block it made
   from `&:sym`, in the buffer Prism parses: flatten marks those BlockNodes
   (sym_proc_block), which a user's own `{ |_spx| _spx.m }` is not. The
   offsets are recorded in increasing order. */
static size_t *g_sym_proc_offs;
static size_t g_sym_proc_n, g_sym_proc_cap;
static void sym_proc_block_at(size_t off) {
  if (g_sym_proc_n == g_sym_proc_cap) {
    size_t nc = g_sym_proc_cap ? g_sym_proc_cap * 2 : 16;
    size_t *no = realloc(g_sym_proc_offs, nc * sizeof(size_t));
    if (!no) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    g_sym_proc_offs = no; g_sym_proc_cap = nc;
  }
  g_sym_proc_offs[g_sym_proc_n++] = off;
}
static int sym_proc_block_starts_at(size_t off) {
  size_t lo = 0, hi = g_sym_proc_n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (g_sym_proc_offs[mid] == off) return 1;
    if (g_sym_proc_offs[mid] < off) lo = mid + 1; else hi = mid;
  }
  return 0;
}

static char *rewrite_syntax_sugar(char *source) {
  /* Rewrite .send(:foo, args) / .send("foo", args) → .foo(args) */
  /* Rewrite &:symbol → { |_spx| _spx.symbol } */
  size_t len = strlen(source);
  size_t cap = (len * 2) + 256;
  char *out = malloc(cap);
  size_t oi = 0;
  size_t i = 0;

  #define OUT_CHAR(c) do { if (oi >= cap - 1) { size_t _nc = cap * 2; char *_no = realloc(out, _nc); if (!_no) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); } out = _no; cap = _nc; } out[oi++] = (c); } while(0)
  #define OUT_STR(s) do { const char *_s = (s); while (*_s) { OUT_CHAR(*_s); _s++; } } while(0)

  /* Rewrite one .send(:foo / .send("foo dispatch. `string_form` is 1
     for the double-quoted variant (requires a closing `"`) and 0 for
     the colon-symbol variant. On mismatch (empty / digit-leading
     name, unclosed quote), emits the prefix verbatim and advances
     past it so the outer loop resumes scanning. The args copy tracks
     `"..."` and `'...'` state with `\` escape skipping so parens
     inside string literals do not prematurely close the call. */
  #define REWRITE_SEND_CALL(prefix_str, prefix_len, string_form) do {     \
    size_t _save_i = i;                                                   \
    i += (prefix_len);                                                    \
    size_t _ns = i;                                                       \
    while (i < len && sp_is_method_name_char(source[i])) i++;             \
    size_t _name_len = i - _ns;                                           \
    int _ok = (_name_len > 0);                                            \
 /* Reject digit-leading names: `.123foo()` is not valid Ruby. */         \
    if (_ok && source[_ns] >= '0' && source[_ns] <= '9') { _ok = 0; }     \
    if (_ok && (string_form)) {                                           \
      if (i >= len || source[i] != '"') { _ok = 0; }                      \
      else { i++; /* skip closing quote */ }                              \
    }                                                                     \
    if (!_ok) { i = _save_i; OUT_STR(prefix_str); i += (prefix_len); }    \
    else {                                                                \
      OUT_CHAR('.');                                                      \
      { size_t _k; for (_k = 0; _k < _name_len; _k++) OUT_CHAR(source[_ns + _k]); } \
      if (i < len && source[i] == ')') {                                  \
        i++; /* no args */                                                \
      } \
      else if (i < len && source[i] == ',') {                           \
        i++;                                                              \
        while (i < len && source[i] == ' ') i++;                          \
        OUT_CHAR('(');                                                    \
        { int _depth = 1; char _qc = 0;                                   \
          while (i < len && _depth > 0) {                                 \
            char _ac = source[i];                                         \
            if (_qc != 0) {                                               \
 /* Inside string: `\` escapes next char, skip it as a pair. */           \
              if (_ac == '\\' && i + 1 < len) {                           \
                OUT_CHAR(_ac); OUT_CHAR(source[i + 1]); i += 2; continue; \
              }                                                            \
              if (_ac == _qc) _qc = 0;                                    \
              OUT_CHAR(_ac); i++; continue;                                \
            }                                                              \
            if (_ac == '"' || _ac == '\'') { _qc = _ac; OUT_CHAR(_ac); i++; continue; } \
            if (_ac == '(') _depth++;                                     \
            else if (_ac == ')') { _depth--; if (_depth == 0) { i++; break; } } \
            OUT_CHAR(_ac); i++;                                           \
          } }                                                             \
        OUT_CHAR(')');                                                    \
      }                                                                   \
    }                                                                     \
  } while(0)

  /* ---- Lexical state: the rewrites below must never fire inside string
     literals, heredoc bodies, or comments (#2181: `&:word` inside a string
     was rewritten, silently changing the string's value). A small state
     machine tracks '...' / "..." / %-literals / heredocs / comments and
     copies their contents verbatim; `#{...}` interpolation re-enters code
     (rewrites stay active there, as in CRuby). Regex literals are not
     modeled (slash/division disambiguation needs full parsing); a `&:word`
     inside a regex literal remains a known residual. */
  enum { LX_SQ, LX_DQ, LX_PV, LX_CODE };
  struct { unsigned char st; char open; char close; int depth; } lstk[64];
  int lsp = 0;   /* 0 = top-level code; frames 1.. are literals/interp */
  struct { char tag[64]; int squig; int interp; } hds[8];
  int nhd = 0, in_hd = 0;

  while (i < len) {
    /* --- heredoc body: verbatim until the terminator line --- */
    if (lsp == 0 && in_hd) {
      if (i == 0 || source[i - 1] == '\n') {
        size_t j = i;
        if (hds[0].squig) while (j < len && (source[j] == ' ' || source[j] == '\t')) j++;
        size_t tl = strlen(hds[0].tag);
        if (j + tl <= len && strncmp(source + j, hds[0].tag, tl) == 0) {
          size_t k2 = j + tl;
          while (k2 < len && (source[k2] == ' ' || source[k2] == '\t' || source[k2] == '\r')) k2++;
          if (k2 >= len || source[k2] == '\n') {
            while (i < len && source[i] != '\n') { OUT_CHAR(source[i]); i++; }
            if (i < len) { OUT_CHAR('\n'); i++; }
            memmove(&hds[0], &hds[1], sizeof(hds[0]) * (size_t)(nhd - 1));
            nhd--;
            if (nhd == 0) in_hd = 0;
            continue;
          }
        }
      }
      if (hds[0].interp && source[i] == '\\' && i + 1 < len) {
        OUT_CHAR(source[i]); OUT_CHAR(source[i + 1]); i += 2; continue;
      }
      if (hds[0].interp && source[i] == '#' && i + 1 < len && source[i + 1] == '{' &&
          lsp < 63) {
        lsp++; lstk[lsp].st = LX_CODE; lstk[lsp].open = 0; lstk[lsp].close = 0; lstk[lsp].depth = 1;
        OUT_CHAR('#'); OUT_CHAR('{'); i += 2; continue;
      }
      OUT_CHAR(source[i]); i++; continue;
    }
    /* --- inside a literal frame --- */
    if (lsp > 0 && lstk[lsp].st != LX_CODE) {
      char cch = source[i];
      if (cch == '\\' && i + 1 < len) { OUT_CHAR(cch); OUT_CHAR(source[i + 1]); i += 2; continue; }
      if (lstk[lsp].st == LX_DQ && cch == '#' && i + 1 < len && source[i + 1] == '{' &&
          lsp < 63) {
        lsp++; lstk[lsp].st = LX_CODE; lstk[lsp].open = 0; lstk[lsp].close = 0; lstk[lsp].depth = 1;
        OUT_CHAR('#'); OUT_CHAR('{'); i += 2; continue;
      }
      if (lstk[lsp].open && cch == lstk[lsp].open) { lstk[lsp].depth++; OUT_CHAR(cch); i++; continue; }
      if (cch == lstk[lsp].close) {
        lstk[lsp].depth--;
        if (lstk[lsp].depth == 0) lsp--;
        OUT_CHAR(cch); i++; continue;
      }
      OUT_CHAR(cch); i++; continue;
    }
    /* --- code (top level or interpolation): interp braces close first --- */
    if (lsp > 0 && lstk[lsp].st == LX_CODE) {
      if (source[i] == '{') lstk[lsp].depth++;
      else if (source[i] == '}') {
        lstk[lsp].depth--;
        if (lstk[lsp].depth == 0) { OUT_CHAR('}'); i++; lsp--; continue; }
      }
    }
    /* line comment (also terminates inside interpolation, as in Ruby) */
    if (source[i] == '#') {
      while (i < len && source[i] != '\n') { OUT_CHAR(source[i]); i++; }
      continue;
    }
    /* =begin/=end block comment at line start */
    if ((i == 0 || source[i - 1] == '\n') && i + 6 <= len &&
        strncmp(source + i, "=begin", 6) == 0) {
      while (i < len) {
        if ((i == 0 || source[i - 1] == '\n') && i + 4 <= len &&
            strncmp(source + i, "=end", 4) == 0) {
          while (i < len && source[i] != '\n') { OUT_CHAR(source[i]); i++; }
          if (i < len) { OUT_CHAR('\n'); i++; }
          break;
        }
        OUT_CHAR(source[i]); i++;
      }
      continue;
    }
    /* `?"` / `?'` / `?#` character literal: copy the pair so the quote or
       hash is not misread as an opener */
    if (source[i] == '?' && i + 1 < len &&
        (source[i + 1] == '"' || source[i + 1] == '\'' ||
         source[i + 1] == '`' || source[i + 1] == '#') &&
        (i + 2 >= len || !sp_is_method_name_char(source[i + 2]))) {
      OUT_CHAR('?'); OUT_CHAR(source[i + 1]); i += 2; continue;
    }
    /* string openers */
    if (source[i] == '\'' && lsp < 63) {
      lsp++; lstk[lsp].st = LX_SQ; lstk[lsp].open = 0; lstk[lsp].close = '\''; lstk[lsp].depth = 1;
      OUT_CHAR(source[i]); i++; continue;
    }
    if ((source[i] == '"' || source[i] == '`') && lsp < 63) {
      lsp++; lstk[lsp].st = LX_DQ; lstk[lsp].open = 0; lstk[lsp].close = source[i]; lstk[lsp].depth = 1;
      OUT_CHAR(source[i]); i++; continue;
    }
    /* Regex literal in expression position: open a frame so its escaped
       quotes (`/\"(.*?)\"/`) do not toggle the string state and poison the
       rest of the scan. Heuristic mirrors Ruby's lexer: a `/` is a regex
       when the previous non-space char can END no value (operator, opener,
       separator, or line start); after an identifier/closing token it is
       division. Interpolation inside the regex rides the LX_DQ frame. */
    if (source[i] == '/' && lsp < 63) {
      size_t backR = oi;
      while (backR > 0 && (out[backR - 1] == ' ' || out[backR - 1] == '\t')) backR--;
      char pvR = backR > 0 ? out[backR - 1] : '\n';
      if (strchr("=(,[{;\n!&|~^+-*%<>?:", pvR)) {
        lsp++; lstk[lsp].st = LX_DQ; lstk[lsp].open = 0; lstk[lsp].close = '/'; lstk[lsp].depth = 1;
        OUT_CHAR(source[i]); i++; continue;
      }
    }
    /* %-literals: %w %W %i %I %q %Q with any delimiter always; a bare
       %<delim> only in operator position (else it is modulo) */
    if (source[i] == '%' && i + 1 < len && lsp < 63) {
      char pm = source[i + 1];
      size_t di = i + 1;
      int interp = 1, mode = 0;
      if (pm == 'w' || pm == 'i' || pm == 'q') { interp = 0; mode = 1; di = i + 2; }
      else if (pm == 'W' || pm == 'I' || pm == 'Q') { interp = 1; mode = 1; di = i + 2; }
      char d = di < len ? source[di] : 0;
      int is_delim = d && !((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                            (d >= '0' && d <= '9') || d == '_' || d == ' ' ||
                            d == '\t' || d == '\n' || d == '\r' || d == '=');
      int valid = 0;
      if (mode) valid = is_delim;
      else if (is_delim) {
        size_t back2 = oi;
        while (back2 > 0 && (out[back2 - 1] == ' ' || out[back2 - 1] == '\t')) back2--;
        char pv = back2 > 0 ? out[back2 - 1] : '\n';
        valid = strchr("=,([{+-*/%|&<>?:;!^\n", pv) != NULL;
      }
      if (valid) {
        char close2 = d == '(' ? ')' : d == '[' ? ']' : d == '{' ? '}' : d == '<' ? '>' : d;
        lsp++;
        lstk[lsp].st = interp ? LX_DQ : LX_PV;
        lstk[lsp].open = (close2 != d) ? d : 0;
        lstk[lsp].close = close2;
        lstk[lsp].depth = 1;
        while (i <= di) { OUT_CHAR(source[i]); i++; }
        continue;
      }
    }
    /* heredoc opener: <<~TAG / <<-TAG / <<'TAG' / <<"TAG" / <<TAG. Tags are
       required to start [A-Z_] in the unquoted forms (lowercase unquoted
       tags stay unmodeled rather than risk misreading a shift). */
    if (source[i] == '<' && i + 1 < len && source[i + 1] == '<' && nhd < 8) {
      size_t j = i + 2;
      int squig = 0;
      if (j < len && (source[j] == '~' || source[j] == '-')) { squig = 1; j++; }
      char q = 0;
      if (j < len && (source[j] == '\'' || source[j] == '"')) { q = source[j]; j++; }
      size_t ts = j;
      while (j < len && ((source[j] >= 'A' && source[j] <= 'Z') ||
                         (source[j] >= 'a' && source[j] <= 'z') ||
                         (source[j] >= '0' && source[j] <= '9') || source[j] == '_')) j++;
      size_t tl2 = j - ts;
      int hd_ok = tl2 > 0 && tl2 < sizeof(hds[0].tag) &&
                  (q || (source[ts] >= 'A' && source[ts] <= 'Z') || source[ts] == '_');
      if (hd_ok && q) {
        if (j < len && source[j] == q) j++;
        else hd_ok = 0;
      }
      if (hd_ok) {
        memcpy(hds[nhd].tag, source + ts, tl2);
        hds[nhd].tag[tl2] = '\0';
        hds[nhd].squig = squig;
        hds[nhd].interp = (q != '\'');
        nhd++;
        while (i < j) { OUT_CHAR(source[i]); i++; }
        continue;
      }
    }
    /* body newline at top level starts any pending heredoc bodies */
    if (source[i] == '\n' && lsp == 0 && nhd > 0) {
      OUT_CHAR('\n'); i++; in_hd = 1; continue;
    }
    /* `.send(...)` is NOT rewritten here (it used to be): the textual pass
       has no types, so it also erased sends on BasicObject subclasses, which
       lack #send and must raise (#2725). The analyze-side send desugar
       retargets it with the receiver's class in hand. */
    /* Load-path manipulation in statement position -- the pre-bundler
       `$:.unshift File.dirname(__FILE__)` preamble and its variants
       (`$LOAD_PATH.unshift/push/append/<<`). Under whole-program AOT there
       is no load path, so the idiom is meaningless rather than wrong:
       warn and blank the statement, symmetric with an unresolvable
       require. Only fires at a statement start on a self-contained line
       (balanced brackets, no continuation tail); a `$:` whose value flows
       into program logic still refuses in analyze. */
    if (source[i] == '$') {
      size_t back4 = oi;
      while (back4 > 0 && (out[back4 - 1] == ' ' || out[back4 - 1] == '\t')) back4--;
      char pv4 = back4 > 0 ? out[back4 - 1] : '\n';
      size_t j4 = i;
      if (strncmp(source + i, "$LOAD_PATH", 10) == 0) j4 = i + 10;
      else if (source[i + 1] == ':' && source[i + 2] != ':') j4 = i + 2;
      if ((pv4 == '\n' || pv4 == ';') && j4 > i) {
        size_t m4 = j4;
        while (m4 < len && source[m4] == ' ') m4++;
        int is_mut = 0;
        if (strncmp(source + m4, ".unshift", 8) == 0 ||
            strncmp(source + m4, ".prepend", 8) == 0) { m4 += 8; is_mut = 1; }
        else if (strncmp(source + m4, ".append", 7) == 0) { m4 += 7; is_mut = 1; }
        else if (strncmp(source + m4, ".push", 5) == 0) { m4 += 5; is_mut = 1; }
        else if (strncmp(source + m4, "<<", 2) == 0) { m4 += 2; is_mut = 1; }
        if (is_mut) {
          /* the rest of the line must be self-contained */
          size_t e4 = m4; int depth4 = 0; char q4 = 0; int ok4 = 1;
          char lastc4 = 0;
          while (e4 < len && source[e4] != '\n') {
            char ch4 = source[e4];
            if (q4) { if (ch4 == q4 && source[e4 - 1] != '\\') q4 = 0; }
            else if (ch4 == '"' || ch4 == '\'') q4 = ch4;
            else if (ch4 == '(' || ch4 == '[') depth4++;
            else if (ch4 == ')' || ch4 == ']') { if (--depth4 < 0) { ok4 = 0; break; } }
            else if (ch4 == '#') break;
            if (ch4 != ' ' && ch4 != '\t') lastc4 = ch4;
            e4++;
          }
          if (ok4 && depth4 == 0 && !q4 && lastc4 &&
              !strchr(",.\\+|&=({[", lastc4)) {
            fprintf(stderr,
                    "warning: load-path manipulation is meaningless in Spinel "
                    "(no load path at runtime); the statement is ignored\n");
            while (e4 < len && source[e4] != '\n') e4++;
            i = e4;
            continue;
          }
        }
      }
    }
    /* Forwardable: `def_delegator :@recv, :meth[, :alias]` and
       `def_delegators :@recv, :m1, :m2, ...` rewrite in place (single line,
       preserving line count) to plain forwarding defs:
       `def alias(*a) = a.length == 0 ? @recv.meth : @recv.meth(*a)`.
       The runtime arity split keeps zero-arg delegation off the splat path
       (a builtin target's optional-arg arm would otherwise see the empty
       rest array as an argument). Block forwarding is not emitted (known
       forwarding gap). The receiver symbol may be an ivar (:@foo) or a
       method (:foo). Only fires at a statement start (preceded by
       newline/semicolon + indentation). def_instance_delegator(s) are
       Forwardable's aliases of the same; SingleForwardable's
       def_single_delegator(s) delegate at the class level (`def self.x`).
       Operator names (:[], :<<, :==) delegate like any other. */
    size_t fwkw = 0; int fwsingle = 0;
    if (strncmp(source + i, "def_delegator", 13) == 0) fwkw = 13;
    else if (strncmp(source + i, "def_instance_delegator", 22) == 0) fwkw = 22;
    else if (strncmp(source + i, "def_single_delegator", 20) == 0) { fwkw = 20; fwsingle = 1; }
    if (fwkw && (i == 0 || !sp_is_method_name_char(source[i - 1]))) {
      size_t back3 = oi;
      while (back3 > 0 && (out[back3 - 1] == ' ' || out[back3 - 1] == '\t')) back3--;
      char pv3 = back3 > 0 ? out[back3 - 1] : '\n';
      int plural = (i + fwkw + 1 <= len && source[i + fwkw] == 's');
      size_t j3 = i + fwkw + (plural ? 1 : 0);
      while (j3 < len && (source[j3] == ' ' || source[j3] == '(')) j3++;
      if ((pv3 == '\n' || pv3 == ';') && j3 < len && source[j3] == ':') {
        /* parse the symbol list. It may continue onto following lines --
           after a trailing comma, inside the call's parentheses, or after a
           backslash -- as CRuby reads it; those newlines are counted and
           re-emitted after the rewrite so the line count is preserved. Only
           the methods on the first line were defined before, and the rest
           were dropped silently or left as a stray expression (#4822). */
        int paren3 = 0;
        { size_t pj = i + fwkw + (plural ? 1 : 0);
          while (pj < j3) { if (source[pj] == '(') paren3++; pj++; } }
        size_t symcap = 16; int nsym = 0;
        char (*syms)[160] = malloc(sizeof(*syms) * symcap);
        if (!syms) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
        int nl3 = 0, bad3 = 0;
        size_t k3 = j3;
        int after_comma = 0;
        while (k3 < len) {
          /* separators, and a line break where the list continues */
          while (k3 < len) {
            char ch = source[k3];
            if (ch == ' ' || ch == '\t' || ch == '\r') { k3++; continue; }
            if (ch == ',') { after_comma = 1; k3++; continue; }
            if (ch == '(') { paren3++; k3++; continue; }
            if (ch == ')') { if (paren3 > 0) paren3--; k3++; continue; }
            if (ch == '\\' && k3 + 1 < len && source[k3 + 1] == '\n') { nl3++; k3 += 2; continue; }
            if (ch == '\n' && (after_comma || paren3 > 0)) { nl3++; k3++; continue; }
            break;
          }
          /* `*NAMES`, a constant holding a literal Symbol Array (Rack::Lint's
             `def_delegators :@stream, *REQUIRED_METHODS`): its names */
          if (k3 + 1 < len && source[k3] == '*' && source[k3 + 1] >= 'A' && source[k3 + 1] <= 'Z' &&
              nsym > 0) {
            size_t c0 = k3 + 1, c1 = c0;
            while (c1 < len && sp_is_method_name_char(source[c1])) c1++;
            char cname[128];
            if (c1 - c0 >= sizeof cname) { bad3 = 1; break; }
            memcpy(cname, source + c0, c1 - c0); cname[c1 - c0] = 0;
            char (*csyms)[160] = NULL; int ncs = 0;
            if (!sp_const_symbol_list(source, len, i, cname, &csyms, &ncs)) { bad3 = 1; break; }
            for (int q = 0; q < ncs; q++) {
              if ((size_t)nsym == symcap) {
                symcap *= 2;
                char (*ns)[160] = realloc(syms, sizeof(*syms) * symcap);
                if (!ns) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
                syms = ns;
              }
              memcpy(syms[nsym++], csyms[q], sizeof csyms[q]);
            }
            free(csyms);
            after_comma = 0;
            k3 = c1;
            continue;
          }
          if (k3 >= len || source[k3] != ':') break;
          after_comma = 0;
          k3++;
          size_t s3 = k3;
          if (k3 < len && source[k3] == '@') k3++;
          while (k3 < len && sp_is_method_name_char(source[k3])) k3++;
          if (k3 == s3) {
            /* operator method symbols: :[] :[]= :<< :== :<=> :+ ... */
            static const char *const fwops[] = { "[]=", "[]", "<=>", "===", "==", "=~",
              "<<", ">>", "<=", ">=", "**", "+@", "-@", "!", "~", "+", "-",
              "*", "/", "%", "<", ">", "&", "|", "^", NULL };
            for (int oq = 0; fwops[oq]; oq++) {
              size_t ol = strlen(fwops[oq]);
              if (k3 + ol <= len && strncmp(source + k3, fwops[oq], ol) == 0) { k3 += ol; break; }
            }
          }
          size_t l3 = k3 - s3;
          if (l3 == 0 || l3 >= sizeof(syms[0])) { bad3 = 1; break; }
          if ((size_t)nsym == symcap) {
            symcap *= 2;
            char (*ns)[160] = realloc(syms, sizeof(*syms) * symcap);
            if (!ns) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
            syms = ns;
          }
          memcpy(syms[nsym], source + s3, l3);
          syms[nsym][l3] = 0;
          nsym++;
        }
        if (bad3) nsym = 0;
        /* trailing junk after the list (a real expression) -> leave alone */
        size_t t3 = k3;
        while (t3 < len && (source[t3] == ' ' || source[t3] == ')' ||
                            source[t3] == '\r')) t3++;
        int clean = paren3 == 0 && !after_comma &&
                    (t3 >= len || source[t3] == '\n' || source[t3] == '#');
        int min_syms = 2;
        int max_syms = plural ? nsym : 3;
        char *line3 = NULL;
        if (nsym >= min_syms && nsym <= max_syms && clean) {
          const char *recvtxt = syms[0];
          size_t cap3 = 64 + (size_t)nsym * 512;
          line3 = malloc(cap3);
          if (!line3) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
          line3[0] = 0;
          /* One forwarding def. A setter alias (`:timer_a=`) cannot be an
             endless def (the grammar forbids `def x=(v) = ...`), so it is a
             one-line classic def whose body is the assignment: the same
             line count, and the target is written as the assignment the
             delegate would make (`@ta.counter = v`), whether or not the
             delegated name is itself a setter (#4518). */
          #define FW_DEF(dst, cap, als, meth) do { \
            size_t _al = strlen(als), _ml = strlen(meth); \
            if (_al > 1 && (als)[_al - 1] == '=' && sp_is_method_name_char((als)[_al - 2])) \
              snprintf(dst, cap, "def %s%s(_fw_v); %s.%.*s = _fw_v; end", fwsingle ? "self." : "", als, recvtxt, \
                       (int)(_ml > 1 && (meth)[_ml - 1] == '=' ? _ml - 1 : _ml), meth); \
            else \
              snprintf(dst, cap, "def %s%s(*_fw_a) = _fw_a.length == 0 ? %s.%s : %s.%s(*_fw_a)", \
                       fwsingle ? "self." : "", als, recvtxt, meth, recvtxt, meth); \
          } while (0)
          if (!plural) {
            const char *meth3 = syms[1];
            const char *als3 = nsym == 3 ? syms[2] : syms[1];
            FW_DEF(line3, cap3, als3, meth3);
          }
          else {
            size_t off3 = 0;
            for (int q3 = 1; q3 < nsym; q3++) {
              if (q3 > 1) { off3 += (size_t)snprintf(line3 + off3, cap3 - off3, "; "); }
              FW_DEF(line3 + off3, cap3 - off3, syms[q3], syms[q3]);
              off3 += strlen(line3 + off3);
            }
          }
          #undef FW_DEF
        }
        free(syms);
        if (line3 && line3[0]) {
          OUT_STR(line3);
          free(line3);
          /* the continuation lines the list spanned, as blank lines */
          for (int q3 = 0; q3 < nl3; q3++) OUT_STR("\n");
          i = t3;   /* resume at end-of-line remainder (newline/comment) */
          continue;
        }
        free(line3);
      }
    }
    /* `.__send__(...)` is not rewritten here either. It used to be, on the
       grounds that __send__ IS a BasicObject method so the blind rewrite is
       always right -- which is true about the RECEIVER and says nothing about
       what the rewrite costs: the textual pass produces a plain `.foo(args)`
       with nothing to mark, and send's whole point is that it ignores
       visibility. The analyze-side desugar retargets it with the receiver's
       class in hand and stamps the permission (see desugar_public_send_recv);
       its blank-slate guard already exempts __send__. */
    /* `&:symbol` symbol-to-proc. Prism parses `&:sym` natively, but
       Spinel only lowers the native BlockArgumentNode+SymbolNode shape
       for a few methods (inject / reduce / sort_by); general block
       takers rely on this textual rewrite to an explicit block. The
       block has to land where Ruby reads it as a block, not a hash:
         - sole parenthesized arg `m(&:s)`   -> `m { |_spx| _spx.s }`
         - after a positional, paren call
           `m(a, &:s)`                       -> `m(a) { |_spx| _spx.s }`
         - after a positional, command call
           `m a, &:s`                        -> `m a do |_spx| _spx.s end`
         - command-position sole arg `m &:s` -> `m  { |_spx| _spx.s }`
       The bare `{ |_spx| ... }` was only correct in block position; a
       `,`-prefixed brace parses as a hash literal, which was the bundler
       (`File.open(f, "r:UTF-8", &:read)`) / minitest
       (`define_method :mu_pp, &:pretty_inspect`) parse failure.

       Operator symbols (`&:+`) have name_len 0 here and are left for
       Prism's native path (the arith reduce/inject lowering handles
       them). Known limitation: a paren-less command carrying a
       symbol-proc that is itself nested as an argument inside a paren
       call -- `p(foo :a, &:s)` -- mis-binds the block to the outer
       call, because textually its args sit at the same paren depth as
       the enclosing call (indistinguishable without re-implementing
       Ruby's command/arg disambiguation). Neither target gem hits
       this; a robust fix is an AST-level lowering, deferred. */
    if (i + 2 < len && source[i] == '&' && source[i + 1] == ':') {
      /* Last emitted non-space char. Skip newlines too so a multi-line
         `m(a,\n  &:s)` still sees the comma. */
      size_t back = oi;
      while (back > 0 && (out[back - 1] == ' ' || out[back - 1] == '\t' ||
                          out[back - 1] == '\n' || out[back - 1] == '\r')) back--;
      char prev = (back > 0) ? out[back - 1] : '\0';
      i += 2;
      size_t ns = i;
      while (i < len && (source[i] == '_' || (source[i] >= 'a' && source[i] <= 'z') ||
             (source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= '0' && source[i] <= '9') ||
             source[i] == '?' || source[i] == '!')) i++;
      size_t name_len = i - ns;
      /* The UNARY operator symbols name real methods -- `:-@`, `:+@`, `:~`
         -- but `_spx.-@` is not something Ruby can parse, so they take the
         prefix spelling instead: `{ |_spx| -_spx }`. Without this they fell
         through to Prism, where nothing lowers them, and `[1,2].map(&:-@)`
         reported `map` itself as undefined (#4300). The BINARY operator
         symbols still go to Prism, where the arithmetic reduce/inject
         lowering handles them with two operands. */
      const char *unary_pfx = NULL;
      if (name_len == 0) {
        static const char *const uo_sym[] = { "-@", "+@", "~", NULL };
        static const char *const uo_pfx[] = { "-",  "+",  "~", NULL };
        for (int uo = 0; uo_sym[uo]; uo++) {
          size_t ul = strlen(uo_sym[uo]);
          if (i + ul <= len && strncmp(source + i, uo_sym[uo], ul) == 0 &&
              !(i + ul < len && sp_is_method_name_char(source[i + ul]))) {
            unary_pfx = uo_pfx[uo]; i += ul; name_len = ul; break;
          }
        }
      }
      if (name_len == 0) {
        /* Operator / empty symbol: leave `&:` for Prism's native path. */
        OUT_STR("&:");
        continue;
      }
      if (i < len && source[i] == '.') {
        /* `&:sym.to_proc` (a call ON the symbol): not the shorthand. Leave
           it for Prism -- the explicit to_proc desugars to a lambda later. */
        OUT_STR("&:");
        i = ns;   /* re-emit the symbol name through the normal path */
        continue;
      }
      if (prev == '(') {
        /* Sole parenthesized arg: drop the `(` (and any trailing ws)
           and the matching `)` so the block binds paren-less. #792:
           tolerate whitespace before the `)`. Restore `i` if no `)`
           follows so trailing content isn't swallowed. */
        oi = back - 1;
        size_t after_sym = i;
        while (i < len && (source[i] == ' ' || source[i] == '\t' ||
                           source[i] == '\n' || source[i] == '\r')) i++;
        if (i < len && source[i] == ')') i++;
        else i = after_sym;
        sym_proc_block_at(oi + 1);
        if (unary_pfx) { OUT_STR(" { |_spx| "); OUT_STR(unary_pfx); OUT_STR("_spx"); }
        else { OUT_STR(" { |_spx| _spx.");
          { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); } }
        OUT_STR(" }");
        continue;
      }
      if (prev == ',') {
        /* After a positional arg. Drop the separating comma and
           relocate the block: a parenthesized call closes first then
           takes a brace block; a command call takes a trailing do..end
           (a brace would still read as a hash). Only consume up to the
           `)`; otherwise restore `i` so the newline/`end` that follows
           a paren-less command is preserved. */
        oi = back - 1;
        size_t after_sym = i;
        while (i < len && (source[i] == ' ' || source[i] == '\t' ||
                           source[i] == '\n' || source[i] == '\r')) i++;
        if (i < len && source[i] == ')') {
          i++;
          OUT_CHAR(')');
          sym_proc_block_at(oi + 1);
          if (unary_pfx) { OUT_STR(" { |_spx| "); OUT_STR(unary_pfx); OUT_STR("_spx"); }
          else { OUT_STR(" { |_spx| _spx.");
            { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); } }
          OUT_STR(" }");
        }
else {
          i = after_sym;
          sym_proc_block_at(oi + 1);
          if (unary_pfx) { OUT_STR(" do |_spx| "); OUT_STR(unary_pfx); OUT_STR("_spx"); }
          else { OUT_STR(" do |_spx| _spx.");
            { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); } }
          OUT_STR(" end");
        }
        continue;
      }
      /* Command-position sole arg (`m &:s`): the brace binds as a
         block (no comma precedes it). */
      sym_proc_block_at(oi + 1);
      OUT_STR(" { |_spx| _spx.");
      { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
      OUT_STR(" }");
      continue;
    }
    OUT_CHAR(source[i]);
    i++;
  }
  out[oi] = '\0';
  free(source);
  return out;
  #undef OUT_CHAR
  #undef OUT_STR
}

/* ---- Main ---- */
/* Parse `source_file` and append the text AST to `out`. `argv0` is the
   invoking program path (used to locate the stdlib for plain `require`s).
   Returns 0 on success, 1 on read/parse error. This is the library copy
   (the in-process lib API; no standalone CLI main). */
static int sp_parse_emit(const char *source_file, const char *argv0, SpStrBuf *out) {
  char *source = read_file(source_file);
  if (!source) {
    fprintf(stderr, "spinel_parse: cannot open '%s'\n", source_file);
    return 1;
  }

  /* `# frozen_string_literal: true` magic comment, per file: the entry file's
     flag is the whole-program fallback; the require resolvers below build the
     per-line table each spliced file's literals are stamped from. */
  g_frozen_string_literal = sp_scan_fsl_pragma(source);

  /* Set the debug flag before resolving requires so the resolvers insert the
     PUSH/POP markers used to rebuild the multi-file source map. */
  {
    const char *dbg = getenv("SPINEL_DEBUG");
    const char *lm = getenv("SPINEL_LINE_MAP");
    int on = (dbg != NULL && dbg[0] == '1' && dbg[1] == '\0')
          || (lm  != NULL && lm[0]  == '1' && lm[1]  == '\0');
    g_emit_line = on ? 1 : 0;
    const char *et = getenv("SPINEL_EMIT_TYPES");
    const char *ww = getenv("SPINEL_WARN_WIDEN");
    g_emit_end = (on && ((et && *et) || (ww && *ww))) ? 1 : 0;
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
  source = sp_splice_object_space(source, argv0, &fsl, &fsl_n);
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
  source = sp_splice_builtins(source, argv0, &fsl, &fsl_n);
  source = sp_splice_builtin_extras(source, argv0, &fsl, &fsl_n);
  source = sp_splice_builtin_enumerator(source, argv0, &fsl, &fsl_n);
  sp_src_lex_drop();

  /* class-body macro calls (module_eval'd templates, computed
     attach_function / const_set names) expanded in place; line count kept */
  { char *mx = sp_expand_class_macros(source);
    if (mx) { free(source); source = mx; } }

  /* Debug: build the buffer-line -> (file, original line) map from the
     marker-annotated buffer *before* syntax-sugar rewriting (which could
     touch a marker line's text). Sugar preserves line count, so the map
     stays aligned with the parsed node lines; if that ever fails to hold,
     disable the map rather than emit wrong attributions. */
  char *premap = NULL;
  if (g_src_map) {
    premap = strdup(source);
    if (!premap) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  source = rewrite_syntax_sugar(source);
  /* Adopt the per-line pragma table only while it provably still lines up
     with the buffer (sugar preserves line count); on mismatch fall back to
     the entry file's flag rather than misattribute. */
  if (sp_count_lines(source) == fsl_n) {
    g_fsl_lines = fsl;
    g_fsl_nlines = fsl_n;
  }
else {
    free(fsl);
    g_fsl_lines = NULL;
    g_fsl_nlines = 0;
  }
  if (g_src_map) {
    size_t la = 1, lb = 1;
    for (const char *p = premap; *p; p++) if (*p == '\n') la++;
    for (const char *p = source; *p; p++) if (*p == '\n') lb++;
    sp_build_line_map(la == lb ? premap : source, source_file);
    if (la != lb) {
      memset(sp_line_orig, 0, sizeof(int) * ((size_t)sp_line_map_n + 2));
      /* Multi-file line attribution unavailable for this program; #line
         falls back to buffer lines. Only worth a word under an explicit
         --debug build (faithful stepping matters there); stay silent for
         the default line-map so normal builds aren't noisy. */
      const char *dbg = getenv("SPINEL_DEBUG");
      if (dbg != NULL && dbg[0] == '1' && dbg[1] == '\0') {
        fprintf(stderr, "spinel_parse: multi-file line map disabled "
                        "(syntax-sugar changed line count)\n");
      }
    }
    free(premap);
  }

  size_t source_len = strlen(source);
  sp_find_builtin_ranges(source);

  /* Parse with Prism */
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, source_len, NULL);
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
    return 1;
  }

  g_parser = &parser;
  g_source_file = source_file;
  g_source_file_escaped = escape_str((const uint8_t *)g_source_file, strlen(g_source_file));

  /* Flatten AST to text */
  lines = NULL;
  line_count = 0;
  line_cap = 0;
  node_counter = 0;

  int root_id = flatten(root);

  /* Output */
  sb_printf(out, "ROOT %d\n", root_id);
  /* Issue #878: emit the source file path as a top-level fact so
     `__dir__` and similar compile-time helpers can recover it
     even when the source contains no `__FILE__` reference. The
     loader stashes it in @source_file_path. */
  sb_printf(out, "SOURCE_FILE %s\n", g_source_file_escaped);
  /* Debug multi-file map: emit the id -> path table so codegen can resolve
     each node's `node_file` id to a path for its `#line` directive. */
  for (int i = 0; i < sp_file_count; i++) {
    char *esc = escape_str((const uint8_t *)sp_file_table[i], strlen(sp_file_table[i]));
    sb_printf(out, "FILE %d %s\n", i, esc);
    free(esc);
  }
  for (size_t i = 0; i < line_count; i++) {
    sb_puts(out, lines[i]);
    sb_puts(out, "\n");
    free(lines[i]);
  }
  free(lines);

  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  free(source);
  free(g_fsl_lines); g_fsl_lines = NULL; g_fsl_nlines = 0;
  free(g_source_file_escaped);  /* paired with the escape_str() in init */
  g_source_file_escaped = NULL;
  sp_includes_free();
  return 0;
}

/* The library copy of the Prism front-end, linked into the C compiler
   (build/spinel): only the in-process lib API, no standalone CLI main. */

/* In-process entry for the single-binary C compiler: parse `source_file`
   and return the text AST as a malloc'd NUL-terminated buffer (caller
   frees), or NULL on error. `argv0` locates the stdlib for plain requires.
   Avoids any on-disk intermediate by writing to an in-memory stream. */
char *sp_parse_file_to_text(const char *source_file, const char *argv0) {
  SpStrBuf out = {0};
  int rc = sp_parse_emit(source_file, argv0, &out);
  if (rc != 0) {
    free(out.data);
    return NULL;
  }
  if (!out.data) {
    /* Success with no bytes emitted: hand back a valid empty string. */
    out.data = (char *)malloc(1);
    if (out.data) out.data[0] = '\0';
  }
  return out.data;
}

/* Parse a Ruby snippet -- a class_eval string a desugar can read at compile
   time -- into the text AST the entry program is loaded from: "ROOT n" and
   the node lines, no file map. NULL when it does not parse. The flattener's
   globals belong to the entry parse, which is over by the time this runs;
   they are saved around the call and reset the way that parse resets them.
   The entry's source map and per-line frozen_string_literal table describe
   the entry buffer, not this text, so they are off for it; a node's line is
   its line in the text (when the entry has lines at all), which the desugar
   moves to the eval site's. NULL as well for text that does not read the
   same spliced into the body (sp_snippet_graftable). */
char *sp_parse_snippet_to_text(const char *src) {
  if (!src || !sp_snippet_graftable(src)) return NULL;
  /* the entry buffer's state the flattener reads: its source map, per-line
     frozen_string_literal table, the `&:sym` block offsets (rewritten
     below for this text), the statement cursor and owner */
  const pm_parser_t *saved_parser = g_parser;
  int saved_emit_line = g_emit_line, saved_emit_end = g_emit_end;
  char **saved_lines = lines; size_t saved_count = line_count, saved_cap = line_cap; int saved_counter = node_counter;
  int saved_map_n = sp_line_map_n; unsigned char *saved_fsl = g_fsl_lines; size_t saved_fsl_n = g_fsl_nlines;
  size_t saved_symp_n = g_sym_proc_n;
  pm_node_t **saved_next = g_stmt_next, **saved_end = g_stmt_end; const uint8_t *saved_owner = g_owner;
  sp_line_map_n = 0; g_fsl_lines = NULL; g_fsl_nlines = 0; g_sym_proc_n = 0;
  g_stmt_next = g_stmt_end = NULL; g_owner = NULL; g_emit_end = 0;
  /* the sugar the entry source went through (`&:sym`, `.send(:m)`), which
     line count it keeps */
  char *buf = strdup(src);
  if (buf) buf = rewrite_syntax_sugar(buf);
  char *res = NULL;
  if (buf) {
    pm_parser_t parser;
    pm_parser_init(&parser, (const uint8_t *)buf, strlen(buf), NULL);
    pm_node_t *root = pm_parse(&parser);
    if (parser.error_list.size == 0) {
      g_parser = &parser;
      lines = NULL; line_count = 0; line_cap = 0; node_counter = 0;
      int root_id = flatten(root);
      SpStrBuf out = {0};
      sb_printf(&out, "ROOT %d\n", root_id);
      for (size_t i = 0; i < line_count; i++) { sb_puts(&out, lines[i]); sb_puts(&out, "\n"); free(lines[i]); }
      free(lines);
      res = out.data;
    }
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(buf);
  }
  lines = saved_lines; line_count = saved_count; line_cap = saved_cap; node_counter = saved_counter;
  g_parser = saved_parser; g_emit_line = saved_emit_line; g_emit_end = saved_emit_end;
  sp_line_map_n = saved_map_n; g_fsl_lines = saved_fsl; g_fsl_nlines = saved_fsl_n; g_sym_proc_n = saved_symp_n;
  g_stmt_next = saved_next; g_stmt_end = saved_end; g_owner = saved_owner;
  return res;
}
