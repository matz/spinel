/* pre_const.c -- compile-time target platform, -D defines, and pure
 * constant-branch evaluation shared by the parser and analysis. */
#include "pre_const.h"
#include "analyze.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  size_t name_len;
  SpPreVal val;
  char *owned_str;
} SpPreDefine;

static char g_target_platform[128] = {0};
static SpPreDefine *g_defines = NULL;
static int g_ndefines = 0;
static int g_cdefines = 0;

static int pre_name_eq(const char *a, size_t alen, const char *b) {
  size_t blen = strlen(b);
  return a && alen == blen && memcmp(a, b, blen) == 0;
}

void sp_pre_set_target_platform(const char *platform) {
  if (!platform || !*platform) {
    g_target_platform[0] = '\0';
    return;
  }
  snprintf(g_target_platform, sizeof g_target_platform, "%s", platform);
}

const char *sp_pre_target_platform(void) {
  return g_target_platform[0] ? g_target_platform : NULL;
}

int sp_pre_target_platform_known(void) {
  return g_target_platform[0] != '\0' && strstr(g_target_platform, "unknown") == NULL;
}

int sp_pre_is_builtin_ruby_const(const char *name, size_t len) {
  static const char *const k_consts[] = {
    "RUBY_ENGINE", "RUBY_PLATFORM", "RUBY_VERSION", "RUBY_ENGINE_VERSION",
    "RUBY_RELEASE_DATE", "RUBY_REVISION", "RUBY_PATCHLEVEL",
    "RUBY_COPYRIGHT", "RUBY_DESCRIPTION", NULL
  };
  for (int i = 0; k_consts[i]; i++)
    if (pre_name_eq(name, len, k_consts[i])) return 1;
  return 0;
}

static int pre_valid_const_name(const char *name, size_t len) {
  if (!name || len == 0 || name[0] < 'A' || name[0] > 'Z') return 0;
  for (size_t i = 1; i < len; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_'))
      return 0;
  }
  return 1;
}

static int pre_parse_int(const char *val, long long *out) {
  if (!val || !*val) return 0;
  if (val[0] == '0' && val[1] == '\0') { *out = 0; return 1; }
  const char *p = val;
  if (*p == '-') p++;
  if (*p < '1' || *p > '9') return 0;
  for (const char *q = p + 1; *q; q++)
    if (*q < '0' || *q > '9') return 0;
  errno = 0;
  char *end = NULL;
  long long v = strtoll(val, &end, 10);
  if (errno != 0 || !end || *end != '\0') return 0;
  *out = v;
  return 1;
}

int sp_pre_define_index(const char *name, size_t len) {
  for (int i = 0; i < g_ndefines; i++)
    if (g_defines[i].name_len == len && memcmp(g_defines[i].name, name, len) == 0)
      return i;
  return -1;
}

int sp_pre_define_cli(const char *spec, char *err, size_t err_cap) {
  if (!spec || !*spec) {
    if (err && err_cap) snprintf(err, err_cap, "-D needs NAME or NAME=VALUE");
    return 0;
  }
  const char *eq = strchr(spec, '=');
  size_t nlen = eq ? (size_t)(eq - spec) : strlen(spec);
  if (!pre_valid_const_name(spec, nlen)) {
    if (err && err_cap)
      snprintf(err, err_cap, "-D expects NAME or NAME=VALUE with a Ruby constant name, got '%s'", spec);
    return 0;
  }
  if (sp_pre_is_builtin_ruby_const(spec, nlen)) {
    if (err && err_cap)
      snprintf(err, err_cap, "-D cannot redefine builtin constant '%.*s'", (int)nlen, spec);
    return 0;
  }
  SpPreVal val;
  memset(&val, 0, sizeof val);
  char *owned = NULL;
  if (!eq) {
    val.kind = SP_PRE_BOOL;
    val.b = 1;
  }
  else {
    const char *v = eq + 1;
    long long iv = 0;
    if (strcmp(v, "true") == 0) {
      val.kind = SP_PRE_BOOL;
      val.b = 1;
    }
    else if (strcmp(v, "false") == 0) {
      val.kind = SP_PRE_BOOL;
      val.b = 0;
    }
    else if (strcmp(v, "nil") == 0) {
      val.kind = SP_PRE_NIL;
    }
    else if (pre_parse_int(v, &iv)) {
      val.kind = SP_PRE_INT;
      val.i = iv;
    }
    else {
      owned = strdup(v);
      if (!owned) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      val.kind = SP_PRE_STR;
      val.s = owned;
      val.slen = strlen(owned);
    }
  }
  int idx = sp_pre_define_index(spec, nlen);
  if (idx >= 0) {
    free(g_defines[idx].owned_str);
    g_defines[idx].val = val;
    g_defines[idx].owned_str = owned;
    return 1;
  }
  if (g_ndefines == g_cdefines) {
    g_cdefines = g_cdefines ? g_cdefines * 2 : 8;
    SpPreDefine *nd = realloc(g_defines, sizeof *nd * (size_t)g_cdefines);
    if (!nd) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    g_defines = nd;
  }
  char *nm = malloc(nlen + 1);
  if (!nm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(nm, spec, nlen);
  nm[nlen] = '\0';
  g_defines[g_ndefines++] = (SpPreDefine){ nm, nlen, val, owned };
  return 1;
}

int sp_pre_define_count(void) { return g_ndefines; }

const char *sp_pre_define_name(int idx) {
  return idx >= 0 && idx < g_ndefines ? g_defines[idx].name : NULL;
}

const SpPreVal *sp_pre_define_val(int idx) {
  return idx >= 0 && idx < g_ndefines ? &g_defines[idx].val : NULL;
}

int sp_pre_lookup_const(const char *name, size_t len,
                        int allow_engine, int allow_platform, int allow_version,
                        const unsigned char *def_shadowed, SpPreVal *out) {
  if (!name || !len || !out) return 0;
  if (allow_engine && pre_name_eq(name, len, "RUBY_ENGINE")) {
    *out = (SpPreVal){ .kind = SP_PRE_STR, .s = "spinel", .slen = 6 };
    return 1;
  }
  if (allow_platform && sp_pre_target_platform_known() && pre_name_eq(name, len, "RUBY_PLATFORM")) {
    *out = (SpPreVal){ .kind = SP_PRE_STR, .s = g_target_platform, .slen = strlen(g_target_platform) };
    return 1;
  }
  if (allow_version && pre_name_eq(name, len, "RUBY_VERSION")) {
    *out = (SpPreVal){ .kind = SP_PRE_STR, .s = SP_RUBY_VERSION, .slen = strlen(SP_RUBY_VERSION) };
    return 1;
  }
  int di = sp_pre_define_index(name, len);
  if (di >= 0 && (!def_shadowed || !def_shadowed[di])) {
    *out = g_defines[di].val;
    return 1;
  }
  return 0;
}

int sp_pre_defined_const_fixed(const char *name, size_t len, int has_remove_const) {
  if (!name || !len || has_remove_const) return -1;
  if (sp_pre_is_builtin_ruby_const(name, len)) return 1;
  if (sp_pre_define_index(name, len) >= 0) return 1;
  /* Core Ruby constants guaranteed present at boot on every target before any
   * require executes. Never return 0 here: an arbitrary constant may be
   * defined by a reachable require or later statement. */
  static const char *const k_core[] = {
    "Object", "Kernel", "BasicObject", "Module", "Class",
    "String", "Integer", "Float", "Numeric", "Array", "Hash", "Symbol",
    "TrueClass", "FalseClass", "NilClass", "Regexp", "Range", "Time",
    "IO", "File", "Dir", "Math", "Comparable", "Enumerable", "Struct", "Data",
    "Proc", "Method", "Exception", "StandardError", "RuntimeError",
    "ArgumentError", "TypeError", "NameError", "NoMethodError", "LoadError",
    "ENV", "ARGV", "STDIN", "STDOUT", "STDERR", NULL
  };
  for (int i = 0; k_core[i]; i++)
    if (pre_name_eq(name, len, k_core[i])) return 1;
  return -1;
}

int sp_pre_is_folded_op(const char *op) {
  if (!op) return 0;
  static const char *const k_ops[] = {
    "==", "!=", "<", "<=", ">", ">=", "!", "nil?",
    "include?", "start_with?", "end_with?", "empty?", "const_defined?", NULL
  };
  for (int i = 0; k_ops[i]; i++)
    if (strcmp(op, k_ops[i]) == 0) return 1;
  return 0;
}

int sp_pre_is_core_op_owner(const char *name, size_t len) {
  if (!name || !len) return 0;
  static const char *const k_owners[] = {
    "String", "Integer", "TrueClass", "FalseClass", "NilClass",
    "Object", "BasicObject", "Kernel", "Comparable", "Module", "Class", NULL
  };
  for (int i = 0; k_owners[i]; i++)
    if (pre_name_eq(name, len, k_owners[i])) return 1;
  return 0;
}

int sp_pre_val_truthy(const SpPreVal *v) {
  if (!v) return 0;
  if (v->kind == SP_PRE_NIL) return 0;
  if (v->kind == SP_PRE_BOOL) return v->b != 0;
  return 1;
}

int sp_pre_eval_unary(const SpPreVal *v, const char *op, SpPreVal *out) {
  if (!v || !op || !out || v->kind == SP_PRE_UNDEF) return 0;
  if (strcmp(op, "!") == 0) {
    *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = !sp_pre_val_truthy(v) };
    return 1;
  }
  if (strcmp(op, "nil?") == 0) {
    *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = v->kind == SP_PRE_NIL };
    return 1;
  }
  if (strcmp(op, "empty?") == 0 && v->kind == SP_PRE_STR) {
    *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = v->slen == 0 };
    return 1;
  }
  return 0;
}

static int pre_str_cmp(const SpPreVal *l, const SpPreVal *r) {
  size_t m = l->slen < r->slen ? l->slen : r->slen;
  int c = m ? memcmp(l->s, r->s, m) : 0;
  if (c != 0) return c < 0 ? -1 : 1;
  return l->slen < r->slen ? -1 : l->slen > r->slen ? 1 : 0;
}

static int pre_str_contains(const SpPreVal *hay, const SpPreVal *ndl) {
  if (ndl->slen == 0) return 1;
  if (ndl->slen > hay->slen) return 0;
  for (size_t i = 0; i <= hay->slen - ndl->slen; i++)
    if (memcmp(hay->s + i, ndl->s, ndl->slen) == 0) return 1;
  return 0;
}

int sp_pre_eval_binary(const SpPreVal *l, const char *op, const SpPreVal *r, SpPreVal *out) {
  if (!l || !r || !op || !out || l->kind == SP_PRE_UNDEF || r->kind == SP_PRE_UNDEF) return 0;
  int is_eq = strcmp(op, "==") == 0;
  int is_ne = !is_eq && strcmp(op, "!=") == 0;
  if (is_eq || is_ne) {
    int same = 0;
    if (l->kind == r->kind) {
      switch (l->kind) {
        case SP_PRE_BOOL: same = l->b == r->b; break;
        case SP_PRE_NIL:  same = 1; break;
        case SP_PRE_INT:  same = l->i == r->i; break;
        case SP_PRE_STR:  same = l->slen == r->slen && (l->slen == 0 || memcmp(l->s, r->s, l->slen) == 0); break;
        default: return 0;
      }
    }
    *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = is_eq ? same : !same };
    return 1;
  }
  if (strcmp(op, "include?") == 0) {
    if (l->kind != SP_PRE_STR || r->kind != SP_PRE_STR) return 0;
    *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = pre_str_contains(l, r) };
    return 1;
  }
  int cmp;
  if (l->kind == SP_PRE_INT && r->kind == SP_PRE_INT) {
    cmp = l->i < r->i ? -1 : l->i > r->i ? 1 : 0;
  }
  else if (l->kind == SP_PRE_STR && r->kind == SP_PRE_STR) {
    cmp = pre_str_cmp(l, r);
  }
  else return 0;
  int b;
  if (strcmp(op, "<") == 0) b = cmp < 0;
  else if (strcmp(op, "<=") == 0) b = cmp <= 0;
  else if (strcmp(op, ">") == 0) b = cmp > 0;
  else if (strcmp(op, ">=") == 0) b = cmp >= 0;
  else return 0;
  *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = b };
  return 1;
}

int sp_pre_eval_str_affix(const SpPreVal *recv, int is_start,
                          const SpPreVal *args, int nargs, SpPreVal *out) {
  if (!recv || recv->kind != SP_PRE_STR || !out || nargs < 0) return 0;
  int matched = 0;
  for (int i = 0; i < nargs; i++) {
    if (args[i].kind != SP_PRE_STR) return 0;
    if (args[i].slen <= recv->slen) {
      const char *at = is_start ? recv->s : recv->s + (recv->slen - args[i].slen);
      if (args[i].slen == 0 || memcmp(at, args[i].s, args[i].slen) == 0) matched = 1;
    }
  }
  *out = (SpPreVal){ .kind = SP_PRE_BOOL, .b = matched };
  return 1;
}
