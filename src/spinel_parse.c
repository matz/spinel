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
static const char *g_source_file = "";
static char *g_source_file_escaped = NULL;  /* escape_str(g_source_file), set once at init */
/* Debug builds: when SPINEL_DEBUG=1, flatten() emits a per-node
   `node_line` field so codegen can place C `#line` directives. Off by
   default so the AST text format (and golden tests) are unchanged. */
static int g_emit_line = 0;
/* Set from the ENTRY file's `# frozen_string_literal: true` magic comment.
   The pragma is per-file in Ruby: the require resolvers build g_fsl_lines
   (one flag byte per line of the final spliced buffer, from each spliced
   file's own head), and flatten() stamps an `fzl` field on string-literal
   nodes from it. This global is the fallback -- used when the per-line
   table is unavailable (syntax-sugar changed the line count) -- and the
   whole-program gate read by the analyzer's mutable-string-buffer pass. */
int g_frozen_string_literal = 0;
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
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_');
      }
      return 0;
    }
    p = nl ? nl + 1 : NULL;
  }
  return 0;
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
static int sp_line_map_n = 0;

static char *cstr(pm_constant_id_t id) {
  if (id == 0) return strdup("");
  pm_constant_t *c = &g_parser->constant_pool.constants[id - 1];
  char *buf = malloc(c->length + 1);
  memcpy(buf, c->start, c->length);
  buf[c->length] = '\0';
  return buf;
}

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
static long long pm_int_value(pm_integer_t *integer) {
  uint64_t val = 0;
  uint64_t max_positive = (uint64_t)LLONG_MAX;
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
    if (overflow || val >= max_negative) return LLONG_MIN;
    return -(long long)val;
  }
  if (overflow || val > max_positive) return LLONG_MAX;
  return (long long)val;
}

/* Whether an integer literal does not fit in a signed 64-bit `value` (so
   pm_int_value saturated it). Mirrors the limb scan above. */
static int pm_int_overflows(pm_integer_t *integer) {
  if (integer->values == NULL) return 0;
  const size_t limb_bits = 32;
  const size_t value_bits = sizeof(uint64_t) * CHAR_BIT;
  uint64_t val = 0;
  int overflow = 0;
  for (size_t i = 0; i < integer->length; i++) {
    if (i >= value_bits / limb_bits) {
      if (integer->values[i] != 0) overflow = 1;
      continue;
    }
    val |= ((uint64_t)integer->values[i]) << (i * limb_bits);
  }
  uint64_t max_positive = (uint64_t)LLONG_MAX;
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

/* ---- Main flattening ---- */
static int flatten(pm_node_t *node) {
  if (!node) return -1;

  int id = node_counter++;
  pm_node_type_t t = PM_NODE_TYPE(node);

  /* Debug builds only: stamp every node with its source line — and, when a
     multi-file map was built, its original file — so codegen can emit
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
    if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n) {
      orig = sp_line_orig[bl];
      fid = sp_line_file[bl];
    }
    emit_int(id, "node_line", (long long)orig);
    emit_int(id, "node_file", (long long)fid);
    /* Column is concatenation-stable (require splicing is line-based), so the
       buffer column equals the original-file column. 0-based, as Prism gives. */
    emit_int(id, "node_col", (long long)lc.column);
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
    N("StatementsNode");
    A("body", &n->body);
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
    /* `class << self; ...; end` — the singleton class block. We
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
    N("CallNode");
    NAME("name", n->name);
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
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_READ_NODE: {
    pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
    N("LocalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
    N("LocalVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
    N("LocalVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
    N("LocalVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)node;
    N("LocalVariableTargetNode");
    NAME("name", n->name);
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
        if (bp->parameters) {
          emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        }
        out_add("R %d %s %d", id, "parameters", bpid);
      }
else if (PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
        pm_numbered_parameters_node_t *np = (pm_numbered_parameters_node_t *)n->parameters;
        int npid = node_counter++;
        out_add("N %d NumberedParametersNode", npid);
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
    I("start_line", (long long)line);
    break;
  }
  case PM_SOURCE_FILE_NODE: {
    /* `__FILE__`. Spinel inlines `require`/`require_relative` at parse
       time so we cannot recover the per-call-site source file; we
       always return the toplevel script path passed to spinel_parse,
       documented in test/source_file.rb. The escaped form is cached
       in g_source_file_escaped at init since it never changes. */
    N("SourceFileNode");
    emit_str(id, "content", g_source_file_escaped);
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
    /* `**h` in argument position — splice a hash's entries as
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
    /* Ruby 3.4 implicit `it` is semantically `_1` — lower to a
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
static int source_references_set(const char *src) {
  for (const char *p = strstr(src, "Set"); p; p = strstr(p + 1, "Set")) {
    char prev = p == src ? 0 : p[-1];
    char next = p[3];
    int prev_ok = prev == 0 ||
                  (!((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
                     (prev >= '0' && prev <= '9') || prev == '_' || prev == ':' || prev == '.'));
    int next_ok = !((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                    (next >= '0' && next <= '9') || next == '_');
    if (prev_ok && next_ok) return 1;
  }
  return strstr(src, ".to_set") != NULL;
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
static char *sp_req_feats[128];
static int sp_req_feats_n = 0;

void sp_feature_mark(const char *name) {
  if (!name) return;
  for (int i = 0; i < sp_req_feats_n; i++)
    if (strcmp(sp_req_feats[i], name) == 0) return;
  if (sp_req_feats_n < 128) sp_req_feats[sp_req_feats_n++] = strdup(name);
}

int sp_feature_enabled(const char *name) {
  if (!g_require_gate) return 1;   /* gate off: always provide (baseline) */
  if (!name) return 1;
  for (int i = 0; i < sp_req_feats_n; i++)
    if (strcmp(sp_req_feats[i], name) == 0) return 1;
  return 0;
}

/* Feature search roots from `-I <dir>` (like ruby -I). A plain `require "X"`
   that is not a bundled lib or native feature is looked up here, in order, as
   <root>/X.rb (CRuby single-file form) or <root>/X/<last>.rb (the colocated
   directory form, where one feature's sources share a directory). */
static char *sp_feature_roots[64];
static int sp_feature_roots_n = 0;

void sp_add_feature_root(const char *dir) {
  if (!dir) return;
  if (sp_feature_roots_n < 64) sp_feature_roots[sp_feature_roots_n++] = strdup(dir);
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

static int sp_path_already_included(const char *canonical) {
  if (!canonical) return 0;
  for (int i = 0; i < sp_included_count; i++) {
    if (sp_included_paths[i] && strcmp(sp_included_paths[i], canonical) == 0) return 1;
  }
  return 0;
}

static void sp_mark_path_included(const char *canonical) {
  if (sp_included_count >= sp_included_cap) {
    int new_cap = sp_included_cap == 0 ? 16 : sp_included_cap * 2;
    char **np = (char **)realloc(sp_included_paths, sizeof(char *) * new_cap);
    if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    sp_included_paths = np;
    sp_included_cap = new_cap;
  }
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
   splice time: a PUSH consumes the parent's (replaced) require line, then
   content lines count within the child's own coordinates until POP. */
#define SP_PUSH_PREFIX "#<SPINEL_PUSH>"
#define SP_POP_PREFIX "#<SPINEL_POP>"

static char **sp_file_table = NULL;  /* id -> path */
static int sp_file_count = 0, sp_file_cap = 0;

static int sp_intern_file(const char *path) {
  for (int i = 0; i < sp_file_count; i++)
    if (strcmp(sp_file_table[i], path) == 0) return i;
  if (sp_file_count >= sp_file_cap) {
    int nc = sp_file_cap == 0 ? 8 : sp_file_cap * 2;
    char **np = (char **)realloc(sp_file_table, sizeof(char *) * nc);
    if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    sp_file_table = np; sp_file_cap = nc;
  }
  char *dup_path = strdup(path);
  if (!dup_path) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_file_table[sp_file_count] = dup_path;
  return sp_file_count++;
}

/* In debug mode, wrap an included file's (already-resolved) content with
   PUSH <path> / POP marker lines. Takes ownership of `content`; returns it
   unchanged when not in debug mode. */
static char *sp_wrap_included(char *content, const char *path) {
  if (!g_emit_line) return content;
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

  int *stk_file = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_next = (int *)malloc(sizeof(int) * (nlines + 2));
  if (!sp_line_file || !sp_line_orig || !stk_file || !stk_next) {
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
    if (strncmp(line, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) == 0) {
      /* The replaced require occupied one parent line: consume it. */
      if (sp >= 0) stk_next[sp] += 1;
      char pathbuf[1024];
      size_t plen = len - strlen(SP_PUSH_PREFIX);
      if (plen >= sizeof(pathbuf)) plen = sizeof(pathbuf) - 1;
      memcpy(pathbuf, line + strlen(SP_PUSH_PREFIX), plen);
      pathbuf[plen] = '\0';
      sp++;
      stk_file[sp] = sp_intern_file(pathbuf);
      stk_next[sp] = 1;
      /* marker line maps to nothing meaningful */
    }
else if (strncmp(line, SP_POP_PREFIX, strlen(SP_POP_PREFIX)) == 0) {
      if (sp > 0) sp--;
    }
else {
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
  if (!g_emit_line) return;
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

  char *result = strdup(source);
  /* One pragma flag per line of `result`, kept in lockstep with every text
     splice below so each literal keeps its own file's flag. */
  size_t fsl_n;
  unsigned char *fsl = sp_fsl_make(source, sp_scan_fsl_pragma(source), &fsl_n);
  char *pos;
  char *scan_from = result;
  while ((pos = strstr(scan_from, "require_relative")) != NULL) {
    /* Check it's at start of line. If the match is mid-line (e.g.
       the word appears in a comment or string), advance past it and
       keep scanning the rest of the file — don't abort the whole
       loop, since later lines may have legitimate require_relative
       statements. */
    if (pos != result && *(pos - 1) != '\n') {
      scan_from = pos + 1;
      continue;
    }
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Extract quoted path */
    char *q1 = strchr(pos, '"');
    char *q2 = strchr(pos, '\'');
    char quote_char;
    char *start;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) {
      quote_char = '"';
      start = q1 + 1;
    }
else if (q2 && q2 < line_end) {
      quote_char = '\'';
      start = q2 + 1;
    }
else { scan_from = pos + 1; continue; }

    char *end = strchr(start, quote_char);
    if (!end || end > line_end) { scan_from = pos + 1; continue; }

    size_t path_len = end - start;
    /* Issue #765: bail rather than silently truncating overlong paths
       into the fixed-size buffers. */
    if (path_len >= sizeof(((struct {char x[512];}*)0)->x)) { scan_from = pos + 1; continue; }
    char rel_path[512];
    snprintf(rel_path, sizeof(rel_path), "%.*s", (int)path_len, start);

    /* Build full path */
    char full_path[1024];
    int fp_n = snprintf(full_path, sizeof(full_path), "%s/%s", dir, rel_path);
    if (fp_n < 0 || (size_t)fp_n >= sizeof(full_path)) { scan_from = pos + 1; continue; }
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

    /* Replace the line */
    size_t line_len = (line_end - pos) + ((*line_end == '\n') ? 1 : 0);
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
    /* Buffer reallocated; restart scan from the top of the new buffer. */
    scan_from = result;
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
  static const char *const natives[] = { "io/console", "monitor", "time", "socket", NULL };
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
  /* "ffi": the builtin FFI DSL accepts the ffi gem's spellings
     (attach_function/callback, :string/:pointer/... types, extend
     FFI::Library as a no-op), so the require is satisfied natively. */
  static const char *const tolerated[] = { "thread", "enumerator", "fiber", "ffi", NULL };
  for (int i = 0; tolerated[i]; i++) {
    if (strcmp(name, tolerated[i]) == 0) return 1;
  }
  return 0;
}

/* ---- Plain require resolution ---- */
static char *resolve_plain_requires(char *source, const char *exe_path,
                                    unsigned char **fsl, size_t *fsl_n) {
  /* Find lib/ relative to this executable, resolving symlinks the same way
     the compiler locates its runtime (#1663): an installed tree runs through
     the /usr/local/bin/spinel symlink, and dirname(argv0) would point the
     bundled-gem lookup at bin/. */
  char lib_dir[1024];
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
    strncpy(lib_dir, self, sizeof(lib_dir) - 1);
    lib_dir[sizeof(lib_dir) - 1] = '\0';
  }
  char *slash = strrchr(lib_dir, '/');
  if (slash) *slash = '\0';
  else strcpy(lib_dir, ".");
  strcat(lib_dir, "/lib");

  char *result = source;
  char *pos;
  /* `scan` lets us step past a computed-argument require without aborting
     the loop; it resets to `result` after each rebuild below. */
  char *scan = result;
  /* Match a `require` at the very start of the buffer (offset 0) or
     immediately after a newline. Re-checking offset 0 every iteration --
     not just while `result == source` -- is what lets a first-line
     `require` still be processed when a later `require` exists: once the
     buffer is rebuilt below, `result != source`, and the old condition
     stranded the line-1 require. */
  for (;;) {
    if (scan == result && strncmp(result, "require ", 8) == 0) {
      pos = result;
    }
else {
      pos = strstr(scan, "\nrequire ");
      if (pos == NULL) break;
      pos++; /* skip the matched newline */
    }
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Only a bare string-literal argument is inlined. A computed argument
       (`require File.expand_path('x', __dir__)`, `require some_const`) is
       left untouched: grabbing the first quote on the line would mistake an
       inner string literal for the lib name and strand the rest of the
       expression, producing invalid syntax. Skip past it and keep
       scanning later lines. (#1383) */
    {
      const char *arg = pos + 8;
      while (arg < line_end && (*arg == ' ' || *arg == '\t')) arg++;
      if (arg >= line_end || (*arg != '"' && *arg != '\'')) {
        scan = line_end;
        continue;
      }
    }

    /* Must be: require "name" or require 'name' */
    char *q1 = strchr(pos + 7, '"');
    char *q2 = strchr(pos + 7, '\'');
    char *start; char quote_char;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) { quote_char = '"'; start = q1 + 1; }
    else if (q2 && q2 < line_end) { quote_char = '\''; start = q2 + 1; }
    else break;
    char *end = strchr(start, quote_char);
    if (!end || end > line_end) break;

    char lib_name[256];
    snprintf(lib_name, sizeof(lib_name), "%.*s", (int)(end - start), start);
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
        snprintf(gp, sizeof(gp), "%.*s/packages/%s/%s.rb", base_len, lib_dir, first, lib_name);
        content = read_file(gp);
        if (!content) {
          /* dev binary one level below the repo root (build/spinel) */
          snprintf(gp, sizeof(gp), "%.*s/../packages/%s/%s.rb", base_len, lib_dir, first, lib_name);
          content = read_file(gp);
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
      }
      if (!content) {
        if (sp_lib_is_native(lib_name)) {
          /* Provided natively by the Spinel runtime; the require is a
             harmless no-op, so don't warn. The require-gate still records it
             so the C-native feature (e.g. io/console) becomes available. */
          sp_feature_mark(lib_name);
          content = strdup("# require provided by Spinel runtime");
        }
else if (sp_require_tolerated(lib_name)) {
          /* A core capability Spinel provides without a file (Thread,
             Enumerator, Fiber, the ffi DSL); the require is a harmless
             no-op, like modern CRuby -- gate or no gate. */
          content = strdup("# require no-op (core feature)");
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
        char *resolved = resolve_requires(content, lib_path, &cfsl, &cfsl_n);
        free(content);
        content = resolved;
      }
    }
    /* Stub arms above leave cfsl NULL: their content is a one-line comment. */
    if (!cfsl) cfsl = sp_fsl_make(content, 0, &cfsl_n);

    /* A trailing same-line modifier (`rescue`, `if`, `unless`, `and`,
       `&&`, ...) puts the require in expression position: `require 'x'
       rescue nil`. Replacing it with inlined statements or a comment
       strands the modifier and Prism reports `unexpected 'rescue'`.
       Substitute a `nil` expression instead so the modifier attaches;
       an optional/unavailable require degrades to nil, which is what the
       `rescue` guards for. `;`, `#`, newline and EOF are statement
       boundaries, not modifiers, and keep the normal inlining path. */
    {
      char *trail = end + 1;
      while (*trail == ' ' || *trail == '\t') trail++;
      /* `\r` is part of a CRLF line ending, not a modifier: treat it as a
         statement boundary so Windows sources keep the normal inlining
         path. */
      if (*trail != '\n' && *trail != '\r' && *trail != ';' &&
          *trail != '\0' && *trail != '#') {
        free(content);
        free(cfsl);   /* same-line `nil` substitution: line count unchanged */
        size_t rl_len = (size_t)((end + 1) - pos);   /* the `require 'x'` span */
        size_t result_len = strlen(result);
        size_t before_len = (size_t)(pos - result);
        char *new_result = malloc(result_len - rl_len + 4);
        memcpy(new_result, result, before_len);
        memcpy(new_result + before_len, "nil", 3);
        memcpy(new_result + before_len + 3, pos + rl_len,
               result_len - before_len - rl_len + 1);
        free(result);
        result = new_result;
        scan = result;
        continue;
      }
    }

    /* Debug: marker-wrap so plain-require'd lib content doesn't corrupt the
       line map's accounting for code after the require. No-op without --debug. */
    content = sp_wrap_included(content, lib_path);
    sp_fsl_pad_wrap(&cfsl, &cfsl_n);

    /* Replace only the `require "name"` statement itself, not the whole
       line, so `require "x"; code` keeps `code`. Consume trailing
       horizontal whitespace and a single terminating newline (so a
       require on its own line leaves no blank line); stop at `;` or any
       other trailing code, which the inserted content's trailing newline
       then pushes onto its own line. */
    char *stmt_end = end + 1;  /* just past the closing quote */
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
    scan = result;
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
       retargets it with the receiver's class in hand. `.__send__` stays: it
       IS a BasicObject method, so the blind rewrite is always right. */
    /* .__send__(:foo, args) → .foo(args). CRuby's overrides-resistant
       alias of send; semantically identical for Spinel's static dispatch. */
    if (i + 11 < len && strncmp(source + i, ".__send__(:", 11) == 0) {
      REWRITE_SEND_CALL(".__send__(:", 11, 0);
      continue;
    }
    /* .__send__("foo", args) → .foo(args). String-form variant. */
    if (i + 11 < len && strncmp(source + i, ".__send__(\"", 11) == 0) {
      REWRITE_SEND_CALL(".__send__(\"", 11, 1);
      continue;
    }
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
        OUT_STR(" { |_spx| _spx.");
        { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
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
          OUT_STR(" { |_spx| _spx.");
          { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
          OUT_STR(" }");
        }
else {
          i = after_sym;
          OUT_STR(" do |_spx| _spx.");
          { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
          OUT_STR(" end");
        }
        continue;
      }
      /* Command-position sole arg (`m &:s`): the brace binds as a
         block (no comma precedes it). */
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
  g_require_gate = getenv("SPINEL_REQUIRE_GATE") ? 1 : 0;
  /* CRuby (3.2+) provides Set without an explicit require. Mirror it: when
     the program references the Set constant or calls to_set and never
     requires "set" (and doesn't define its own Set), prepend the require so
     the bundled shim splices ahead of its uses -- class-method call typing
     reads definitions in document order. The require-splice line map keeps
     diagnostics attributed to the right user lines. The checks are textual,
     so a `require "set"` appearing only in a comment also suppresses the
     splice -- acceptable for a convenience heuristic. */
  if (!strstr(source, "require \"set\"") && !strstr(source, "require 'set'") &&
      !strstr(source, "class Set") && source_references_set(source)) {
    const char *head = "require \"set\"\n";
    size_t sl = strlen(source), hl = strlen(head);
    char *ns = (char *)malloc(sl + hl + 1);
    if (ns) {
      memcpy(ns, head, hl);
      memcpy(ns + hl, source, sl + 1);
      free(source);
      source = ns;
    }
  }
  unsigned char *fsl = NULL; size_t fsl_n = 0;
  char *resolved = resolve_requires(source, source_file, &fsl, &fsl_n);
  free(source);
  source = resolve_plain_requires(resolved, argv0, &fsl, &fsl_n);

  /* Debug: build the buffer-line -> (file, original line) map from the
     marker-annotated buffer *before* syntax-sugar rewriting (which could
     touch a marker line's text). Sugar preserves line count, so the map
     stays aligned with the parsed node lines; if that ever fails to hold,
     disable the map rather than emit wrong attributions. */
  char *premap = NULL;
  if (g_emit_line) {
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
  if (g_emit_line) {
    size_t la = 1, lb = 1;
    for (const char *p = premap; *p; p++) if (*p == '\n') la++;
    for (const char *p = source; *p; p++) if (*p == '\n') lb++;
    if (la == lb) {
      sp_build_line_map(premap, source_file);
    }
else {
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

  /* Parse with Prism */
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, source_len, NULL);
  pm_node_t *root = pm_parse(&parser);

  if (parser.error_list.size > 0) {
    fprintf(stderr, "Parse errors in '%s':\n", source_file);
    pm_diagnostic_t *diag;
    for (diag = (pm_diagnostic_t *)parser.error_list.head; diag; diag = (pm_diagnostic_t *)diag->node.next) {
      fprintf(stderr, "  %s\n", diag->message);
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
