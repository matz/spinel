/* builtin_ops.c -- the builtin method rows (see builtin_ops.h). */

#include <stdlib.h>
#include <string.h>
#include "builtin_ops.h"

/* Rows grouped by receiver kind. Within a kind the order does not matter:
   lookups go through the sorted index below. */
static const BuiltinOp bop_rows[] = {
  /* Process::Tms: four cumulative CPU times, all Float (#3044), fields of
     the by-value struct */
  { TY_TMS, "utime",  0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).utime" },
  { TY_TMS, "stime",  0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).stime" },
  { TY_TMS, "cutime", 0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).cutime" },
  { TY_TMS, "cstime", 0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).cstime" },

  /* Process::Status (sp_ProcessStatus *). The runtime helpers take the
     status word and answer unboxed scalars, -1 being nil for exitstatus
     and termsig; the call site boxes by the inferred type. #class and
     #inspect type as String here, as they always have. */
  { TY_PROCESS_STATUS, "signaled?",  0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_signaled_p(($r)->status)" },
  { TY_PROCESS_STATUS, "exited?",    0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_exited_p(($r)->status)" },
  { TY_PROCESS_STATUS, "coredump?",  0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_coredump_p(($r)->status)" },
  { TY_PROCESS_STATUS, "success?",   0, 0, BF_ANY, TY_POLY,   BOPE_PSTATUS_SUCCESS },
  { TY_PROCESS_STATUS, "exitstatus", 0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_process_status_exitstatus(($r)->status)" },
  { TY_PROCESS_STATUS, "termsig",    0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_process_status_termsig(($r)->status)" },
  { TY_PROCESS_STATUS, "pid",        0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->pid" },
  { TY_PROCESS_STATUS, "to_s",       0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_process_status_to_s(($r)->status, 0)" },
  { TY_PROCESS_STATUS, "inspect",    0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_process_status_to_s(($r)->status, 1)" },
  { TY_PROCESS_STATUS, "class",      0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "((sp_Class){(sp_int)-163, NULL})" },
  { TY_PROCESS_STATUS, "==",         0, 0, BF_ANY, TY_BOOL,    BOPE_PSTATUS_EQ },
  { TY_PROCESS_STATUS, "eql?",       0, 0, BF_ANY, TY_UNKNOWN, BOPE_PSTATUS_EQ },

  /* Socket::Option. Spinel carries the integer-valued options only, so the
     readers answer through the int the option holds. #class is typed here
     and emitted by the generic class arm. */
  { TY_SOCKOPT, "int",     0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->value" },
  { TY_SOCKOPT, "bool",    0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r)->value != 0)" },
  { TY_SOCKOPT, "level",   0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->level" },
  { TY_SOCKOPT, "optname", 0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->optname" },
  { TY_SOCKOPT, "family",  0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->family" },
  { TY_SOCKOPT, "inspect", 0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_sockopt_inspect($r)" },
  { TY_SOCKOPT, "to_s",    0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_sockopt_inspect($r)" },
  { TY_SOCKOPT, "class",   0, 0, BF_ANY, TY_CLASS,  BOPE_NONE },

  /* Addrinfo: the value is immutable, so each reader is a field read. The
     family tests use strcmp, not sp_str_eq: sp_str_eq confirms a hit by
     comparing byte lengths, and the length of a bare C literal is read
     from its s[-1] marker, out of bounds; afname is NUL-free. */
  { TY_ADDRINFO, "ip_address",   0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "($r)->ip" },
  { TY_ADDRINFO, "unix_path",    0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "($r)->ip" },
  { TY_ADDRINFO, "afamily_name", 0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "($r)->afname" },
  { TY_ADDRINFO, "afamily",      0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->afamily" },
  { TY_ADDRINFO, "pfamily",      0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->afamily" },
  { TY_ADDRINFO, "ip_port",      0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->port" },
  { TY_ADDRINFO, "socktype",     0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->socktype" },
  { TY_ADDRINFO, "protocol",     0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r)->protocol" },
  { TY_ADDRINFO, "ipv4?",        0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r)->afname && strcmp(($r)->afname, \"AF_INET\") == 0)" },
  { TY_ADDRINFO, "ipv6?",        0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r)->afname && strcmp(($r)->afname, \"AF_INET6\") == 0)" },
  { TY_ADDRINFO, "unix?",        0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r)->afname && strcmp(($r)->afname, \"AF_UNIX\") == 0)" },
  { TY_ADDRINFO, "ip?",          0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(!(($r)->afname && strcmp(($r)->afname, \"AF_UNIX\") == 0))" },
  { TY_ADDRINFO, "to_sockaddr",  0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_addrinfo_to_sockaddr($r)" },
  { TY_ADDRINFO, "inspect",      0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_addrinfo_inspect($r)" },
  { TY_ADDRINFO, "to_s",         0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_addrinfo_inspect($r)" },
  { TY_ADDRINFO, "class",        0, 0, BF_ANY, TY_CLASS,  BOPE_NONE },
};
#define BOP_NROWS ((int)(sizeof bop_rows / sizeof bop_rows[0]))

/* Row indices sorted by (receiver kind, name), built on first use. */
static int bop_index[BOP_NROWS];
static int bop_indexed;

static int bop_cmp_key(TyKind rt, const char *name, const BuiltinOp *r) {
  if (rt != r->recv) return rt < r->recv ? -1 : 1;
  return strcmp(name, r->name);
}

static int bop_sort_cmp(const void *a, const void *b) {
  const BuiltinOp *x = &bop_rows[*(const int *)a], *y = &bop_rows[*(const int *)b];
  int r = bop_cmp_key(x->recv, x->name, y);
  return r ? r : x->argc_min - y->argc_min;
}

static void bop_build_index(void) {
  for (int i = 0; i < BOP_NROWS; i++) bop_index[i] = i;
  qsort(bop_index, BOP_NROWS, sizeof bop_index[0], bop_sort_cmp);
  bop_indexed = 1;
}

static int bop_row_fits(const BuiltinOp *r, int argc, int has_block) {
  if (argc < r->argc_min || argc > r->argc_max) return 0;
  if (r->block == BF_NONE && has_block) return 0;
  if (r->block == BF_REQUIRED && !has_block) return 0;
  return 1;
}

const BuiltinOp *bop_find(TyKind rt, const char *name, int argc, int has_block) {
  if (!name) return NULL;
  if (!bop_indexed) bop_build_index();
  int lo = 0, hi = BOP_NROWS;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (bop_cmp_key(rt, name, &bop_rows[bop_index[mid]]) > 0) lo = mid + 1;
    else hi = mid;
  }
  /* rows of one name differ by arity or block form: take the first that fits */
  for (int i = lo; i < BOP_NROWS; i++) {
    const BuiltinOp *r = &bop_rows[bop_index[i]];
    if (bop_cmp_key(rt, name, r) != 0) break;
    if (bop_row_fits(r, argc, has_block)) return r;
  }
  return NULL;
}

int bop_covers(TyKind rt) {
  for (int i = 0; i < BOP_NROWS; i++) if (bop_rows[i].recv == rt) return 1;
  return 0;
}
