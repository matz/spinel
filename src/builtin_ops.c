/* builtin_ops.c -- the builtin method rows (see builtin_ops.h). */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "builtin_ops.h"

/* Rows grouped by receiver kind. Within a kind the order does not matter:
   lookups go through the sorted index below. */
static const BuiltinOp bop_rows[] = {
  /* Process::Tms: four cumulative CPU times, all Float (#3044), fields of
     the by-value struct */
  { TY_TMS, "utime",  0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).utime", 0, 0, BOPF_BOXED },
  { TY_TMS, "stime",  0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).stime", 0, 0, BOPF_BOXED },
  { TY_TMS, "cutime", 0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).cutime", 0, 0, BOPF_BOXED },
  { TY_TMS, "cstime", 0, 0, BF_ANY, TY_FLOAT, BOPE_TEMPLATE, "($r).cstime", 0, 0, BOPF_BOXED },

  /* Process::Status (sp_ProcessStatus *). The runtime helpers take the
     status word and answer unboxed scalars, -1 being nil for exitstatus
     and termsig; the call site boxes by the inferred type. #class is the
     class value; it was typed String while no value had this kind, and
     `p $?.class` passed the sp_Class to a string printer. */
  { TY_PROCESS_STATUS, "signaled?",  0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_signaled_p(sp_process_status_recv($r, \"signaled?\")->status)", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "exited?",    0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_exited_p(sp_process_status_recv($r, \"exited?\")->status)", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "coredump?",  0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_process_status_coredump_p(sp_process_status_recv($r, \"coredump?\")->status)", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "success?",   0, 0, BF_ANY, TY_POLY,   BOPE_PSTATUS_SUCCESS, NULL, 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "exitstatus", 0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_process_status_exitstatus(sp_process_status_recv($r, \"exitstatus\")->status)", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "termsig",    0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_process_status_termsig(sp_process_status_recv($r, \"termsig\")->status)", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "pid",        0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_process_status_recv($r, \"pid\")->pid", 0, 0, BOPF_BOXED },
  { TY_PROCESS_STATUS, "to_s",       0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_process_status_str($r, 0)" },
  { TY_PROCESS_STATUS, "inspect",    0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_process_status_str($r, 1)" },
  { TY_PROCESS_STATUS, "class",      0, 0, BF_ANY, TY_CLASS,  BOPE_TEMPLATE, "({ sp_ProcessStatus *_t$t = $r; _t$t ? ((sp_Class){(sp_int)-163, NULL}) : ((sp_Class){(sp_int)-1, SPL(\"NilClass\")}); })" },
  { TY_PROCESS_STATUS, "==",         0, 0, BF_ANY, TY_BOOL,    BOPE_PSTATUS_EQ },
  { TY_PROCESS_STATUS, "eql?",       0, 0, BF_ANY, TY_UNKNOWN, BOPE_PSTATUS_EQ },
  /* `$?` is nil (NULL) before any child has been waited for: the readers
     above raise NoMethodError for it (sp_process_status_recv), and to_s,
     inspect, to_i, == and != below answer as nil does */
  { TY_PROCESS_STATUS, "nil?",       0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r) == NULL)" },
  /* the raw status word, and Ruby 3.2's #== against it (`$? == 0`) */
  { TY_PROCESS_STATUS, "to_i",       0, 0, BF_ANY, TY_INT,    BOPE_TEMPLATE, "({ sp_ProcessStatus *_t$t = $r; _t$t ? _t$t->status : 0; })" },
  { TY_PROCESS_STATUS, "==",         1, 1, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "({ sp_ProcessStatus *_t$t = $r; sp_int _t$u = $i0; _t$t && _t$t->status == _t$u; })", BOP_K(TY_INT) },
  { TY_PROCESS_STATUS, "!=",         1, 1, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "({ sp_ProcessStatus *_t$t = $r; sp_int _t$u = $i0; !_t$t || _t$t->status != _t$u; })", BOP_K(TY_INT) },

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

  /* Time (sp_Time, a value): readers of the receiver, rendered once. Each
     ignores its arguments, as the arms they replace did; #class is typed as
     String here, as it always has been. */
  { TY_TIME, "year",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_year($r)" },
  { TY_TIME, "mon",        0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_mon($r)" },
  { TY_TIME, "month",      0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_mon($r)" },
  { TY_TIME, "day",        0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_mday($r)" },
  { TY_TIME, "mday",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_mday($r)" },
  { TY_TIME, "hour",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_hour($r)" },
  { TY_TIME, "min",        0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_min($r)" },
  { TY_TIME, "sec",        0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_sec($r)" },
  { TY_TIME, "wday",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_wday($r)" },
  { TY_TIME, "yday",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_yday($r)" },
  { TY_TIME, "to_i",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r).tv_sec" },
  { TY_TIME, "tv_sec",     0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "($r).tv_sec" },
  { TY_TIME, "tv_usec",    0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "((sp_int)($r).tv_nsec / 1000)" },
  { TY_TIME, "usec",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "((sp_int)($r).tv_nsec / 1000)" },
  { TY_TIME, "tv_nsec",    0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "((sp_int)($r).tv_nsec)" },
  { TY_TIME, "nsec",       0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "((sp_int)($r).tv_nsec)" },
  { TY_TIME, "utc?",       0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r).is_utc == 1)" },
  { TY_TIME, "gmt?",       0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r).is_utc == 1)" },
  { TY_TIME, "dst?",       0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_isdst($r) != 0)" },
  { TY_TIME, "isdst",      0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_isdst($r) != 0)" },
  { TY_TIME, "utc_offset", 0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_utc_offset($r)" },
  { TY_TIME, "gmt_offset", 0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_utc_offset($r)" },
  { TY_TIME, "gmtoff",     0, 127, BF_ANY, TY_INT,    BOPE_TEMPLATE, "sp_time_utc_offset($r)" },
  { TY_TIME, "inspect",    0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_time_inspect_v($r)" },
  { TY_TIME, "to_s",       0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_time_to_s_v($r)" },
  { TY_TIME, "zone",       0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_time_zone($r)" },
  { TY_TIME, "class",      0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "((sp_Class){(sp_int)-1, SPL(\"Time\")})" },
  { TY_TIME, "getgm",      0, 127, BF_ANY, TY_TIME,   BOPE_TEMPLATE, "sp_time_utc($r)" },
  { TY_TIME, "sunday?",    0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 0)" },
  { TY_TIME, "monday?",    0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 1)" },
  { TY_TIME, "tuesday?",   0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 2)" },
  { TY_TIME, "wednesday?", 0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 3)" },
  { TY_TIME, "thursday?",  0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 4)" },
  { TY_TIME, "friday?",    0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 5)" },
  { TY_TIME, "saturday?",  0, 127, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(sp_time_wday($r) == 6)" },
  { TY_TIME, "asctime",    0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_time_strftime($r, \"%a %b %e %H:%M:%S %Y\")" },
  { TY_TIME, "ctime",      0, 127, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_time_strftime($r, \"%a %b %e %H:%M:%S %Y\")" },

  /* MatchData (sp_MatchData *, NULL for no match): the readers of the
     receiver, rendered once; the BOPE_NONE rows type calls whose arms render
     an argument and stay in emit_value_recv_call. #[] and
     named_captures(symbolize_names:) are typed by the argument there. A
     name with no row is refused unless Object is reopened with it. */
  { TY_MATCHDATA, "inspect",          0,   0, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_inspect($r)" },  /* #2500 */
  { TY_MATCHDATA, "to_s",             0, 127, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_to_s($r)" },
  { TY_MATCHDATA, "pre_match",        0, 127, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_pre_match($r)" },
  { TY_MATCHDATA, "post_match",       0, 127, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_post_match($r)" },
  { TY_MATCHDATA, "string",           0,   0, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_string($r)" },
  { TY_MATCHDATA, "string",           1, 127, BF_ANY, TY_STRING,         BOPE_NONE },
  { TY_MATCHDATA, "names",            0,   0, BF_ANY, TY_STR_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_names($r)" },
  { TY_MATCHDATA, "names",            1, 127, BF_ANY, TY_STR_ARRAY,      BOPE_NONE },
  { TY_MATCHDATA, "regexp",           0,   0, BF_ANY, TY_REGEX,          BOPE_TEMPLATE, "((mrb_regexp_pattern *)($r)->pat)" },  /* #2499 */
  { TY_MATCHDATA, "length",           0,   0, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_length($r)" },
  { TY_MATCHDATA, "length",           1, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "size",             0,   0, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_length($r)" },
  { TY_MATCHDATA, "size",             1, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "captures",         0, 127, BF_ANY, TY_POLY_ARRAY,     BOPE_TEMPLATE, "sp_MatchData_captures($r)" },
  { TY_MATCHDATA, "to_a",             0, 127, BF_ANY, TY_POLY_ARRAY,     BOPE_TEMPLATE, "sp_MatchData_to_a($r)" },
  { TY_MATCHDATA, "deconstruct",      0,   0, BF_ANY, TY_POLY_ARRAY,     BOPE_TEMPLATE, "sp_MatchData_captures($r)" },
  { TY_MATCHDATA, "values_at",        0,   0, BF_ANY, TY_POLY_ARRAY,     BOPE_TEMPLATE, "((void)($r), sp_PolyArray_new())" },  /* selects nothing (#3846) */
  { TY_MATCHDATA, "values_at",        1, 127, BF_ANY, TY_POLY_ARRAY,     BOPE_NONE },
  { TY_MATCHDATA, "named_captures",   0,   0, BF_ANY, TY_STR_POLY_HASH,  BOPE_TEMPLATE, "sp_md_named_captures($r)" },
  { TY_MATCHDATA, "named_captures",   2, 127, BF_ANY, TY_STR_POLY_HASH,  BOPE_NONE },
  { TY_MATCHDATA, "nil?",             0, 127, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "($r == 0)" },
  { TY_MATCHDATA, "hash",             0,   0, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_hash($r)" },  /* content-based (#3014) */
  { TY_MATCHDATA, "frozen?",          0,   0, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "sp_gc_is_frozen((void *)($r))" },  /* the bit freeze sets (#3638) */
  { TY_MATCHDATA, "freeze",           0,   0, BF_ANY, TY_MATCHDATA,      BOPE_TEMPLATE, "((sp_MatchData *)sp_gc_freeze((void *)($r)))" },
  { TY_MATCHDATA, "==",              1,   1, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "sp_MatchData_eq($r, $e0)", BOP_K(TY_MATCHDATA) },
  { TY_MATCHDATA, "==",              1,   1, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "((void)($r), (void)($b0), 0)", TY_UNKNOWN },
  { TY_MATCHDATA, "eql?",            1,   1, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "sp_MatchData_eq($r, $e0)", BOP_K(TY_MATCHDATA) },
  { TY_MATCHDATA, "eql?",            1,   1, BF_ANY, TY_BOOL,           BOPE_TEMPLATE, "((void)($r), (void)($b0), 0)", TY_UNKNOWN },
  { TY_MATCHDATA, "match",           1,   1, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_aref_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "match",           1,   1, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_aref_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "match",           1,   1, BF_ANY, TY_STRING,         BOPE_TEMPLATE, "sp_MatchData_aref($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "match_length",    1,   1, BF_ANY, TY_POLY,           BOPE_TEMPLATE, "sp_MatchData_match_length_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "match_length",    1,   1, BF_ANY, TY_POLY,           BOPE_TEMPLATE, "sp_MatchData_match_length_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "match_length",    1,   1, BF_ANY, TY_POLY,           BOPE_TEMPLATE, "sp_MatchData_match_length($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "deconstruct_keys", 1,   1, BF_ANY, TY_SYM_POLY_HASH,  BOPE_TEMPLATE, "sp_md_deconstruct_keys($r, $b0)", TY_UNKNOWN },
  { TY_MATCHDATA, "begin",           1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_begin_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "begin",           1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_begin_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "begin",           1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_begin($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "end",             1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_end_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "end",             1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_end_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "end",             1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_end($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "offset",          1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_offset_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "offset",          1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_offset_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "offset",          1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_offset($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "bytebegin",       1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_bytebegin_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "bytebegin",       1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_bytebegin_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "bytebegin",       1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_bytebegin($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "byteend",         1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_byteend_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "byteend",         1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_byteend_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "byteend",         1,   1, BF_ANY, TY_INT,            BOPE_TEMPLATE, "sp_MatchData_byteend($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "byteoffset",      1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_byteoffset_name($r, sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_MATCHDATA, "byteoffset",      1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_byteoffset_name($r, $e0)", BOP_K(TY_STRING) },
  { TY_MATCHDATA, "byteoffset",      1,   1, BF_ANY, TY_INT_ARRAY,      BOPE_TEMPLATE, "sp_MatchData_byteoffset($r, $i0)", TY_UNKNOWN },
  { TY_MATCHDATA, "begin",            0, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "end",              0, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "bytebegin",        0, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "byteend",          0, 127, BF_ANY, TY_INT,            BOPE_NONE },
  { TY_MATCHDATA, "offset",           0, 127, BF_ANY, TY_INT_ARRAY,      BOPE_NONE },
  { TY_MATCHDATA, "byteoffset",       0, 127, BF_ANY, TY_INT_ARRAY,      BOPE_NONE },

  /* Complex (sp_Complex, by value: re, im and the fl bits that say which
     component is Integer-classed). real/imaginary/abs box to poly, each
     component keeping its CRuby class. % and modulo raise NoMethodError
     and are typed Complex only so the raise has a consistent slot (#2618).
     nonzero? is self or nil; infinite? and <=> answer nil through an
     Integer sentinel. Inference typed most names for any arity where
     codegen emits one, so such a name has its codegen rows and a wider
     BOPE_NONE row after them. */
#define CX_NUM  (BOP_K(TY_COMPLEX) | BOP_K(TY_INT) | BOP_K(TY_FLOAT) | BOP_K(TY_RATIONAL) | BOP_K(TY_POLY))
#define CX_TYPEERROR "((void)($r), (void)($e0), (sp_raise_cls(\"TypeError\", \"can't be coerced into Complex\"), (sp_Complex){0,0,0}))"
  { TY_COMPLEX, "real",        0, 127, BF_ANY, TY_POLY,       BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_complex_comp_v(_t$t.re, _t$t.fl & SP_CPLX_RE_F); })" },
  { TY_COMPLEX, "imaginary",   0, 127, BF_ANY, TY_POLY,       BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_complex_comp_v(_t$t.im, _t$t.fl & SP_CPLX_IM_F); })" },
  { TY_COMPLEX, "imag",        0, 127, BF_ANY, TY_POLY,       BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_complex_comp_v(_t$t.im, _t$t.fl & SP_CPLX_IM_F); })" },
  { TY_COMPLEX, "conjugate",   0, 127, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "sp_complex_conjugate($r)" },
  { TY_COMPLEX, "conj",        0, 127, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "sp_complex_conjugate($r)" },
  /* abs/abs2: the CRuby class depends on the component classes (Integer
     via the zero-component shortcut / all-Integer abs2) */
  { TY_COMPLEX, "abs",         0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "sp_complex_abs_v($r)" },
  { TY_COMPLEX, "abs",         0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_COMPLEX, "magnitude",   0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "sp_complex_abs_v($r)" },
  { TY_COMPLEX, "magnitude",   0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_COMPLEX, "abs2",        0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "sp_complex_abs2_v($r)" },
  { TY_COMPLEX, "abs2",        0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_COMPLEX, "arg",         0,   0, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "({ sp_Complex _t$t = $r; atan2(_t$t.im, _t$t.re); })" },
  { TY_COMPLEX, "arg",         0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  { TY_COMPLEX, "angle",       0,   0, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "({ sp_Complex _t$t = $r; atan2(_t$t.im, _t$t.re); })" },
  { TY_COMPLEX, "angle",       0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  { TY_COMPLEX, "phase",       0,   0, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "({ sp_Complex _t$t = $r; atan2(_t$t.im, _t$t.re); })" },
  { TY_COMPLEX, "phase",       0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  /* instance #polar ([abs, arg]) and #rect ([re, im]): poly pairs, since
     each element's class follows its component */
  { TY_COMPLEX, "polar",       0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new();"
    " sp_PolyArray_push(_t$u, sp_complex_abs_v(_t$t));"
    " sp_PolyArray_push(_t$u, sp_box_float(atan2(_t$t.im, _t$t.re))); _t$u; })" },
  { TY_COMPLEX, "polar",       0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_COMPLEX, "rectangular", 0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new();"
    " sp_PolyArray_push(_t$u, sp_complex_comp_v(_t$t.re, _t$t.fl & SP_CPLX_RE_F));"
    " sp_PolyArray_push(_t$u, sp_complex_comp_v(_t$t.im, _t$t.fl & SP_CPLX_IM_F)); _t$u; })" },
  { TY_COMPLEX, "rect",        0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new();"
    " sp_PolyArray_push(_t$u, sp_complex_comp_v(_t$t.re, _t$t.fl & SP_CPLX_RE_F));"
    " sp_PolyArray_push(_t$u, sp_complex_comp_v(_t$t.im, _t$t.fl & SP_CPLX_IM_F)); _t$u; })" },
  { TY_COMPLEX, "-@",          0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "sp_complex_neg($r)" },
  { TY_COMPLEX, "-@",          0, 127, BF_ANY, TY_COMPLEX,    BOPE_NONE },
  { TY_COMPLEX, "+@",          0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "$r" },
  { TY_COMPLEX, "+@",          0, 127, BF_ANY, TY_COMPLEX,    BOPE_NONE },
  { TY_COMPLEX, "to_c",        0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "$r" },
  { TY_COMPLEX, "to_c",        0, 127, BF_ANY, TY_COMPLEX,    BOPE_NONE },
  { TY_COMPLEX, "to_s",        0, 127, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_complex_to_s($r)" },
  { TY_COMPLEX, "inspect",     0, 127, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_complex_inspect($r)" },
  /* arithmetic by the operand's kind. Dividing by a real scalar divides
     each component: a Float divisor yields Infinity at 0 (IEEE), an Integer
     divisor raises ZeroDivisionError at 0; a boxed divisor makes the same
     choice at run time (the conjugate formula would answer NaN+NaN*i for
     a boxed zero). Any other numeric operand is lifted to a Complex; a
     non-numeric one raises TypeError, not a compile abort (#2963). A poly
     exponent is not modeled here. */
  { TY_COMPLEX, "/",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_div_real($r, (sp_float)($e0))", BOP_K(TY_FLOAT) },
  { TY_COMPLEX, "/",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_div_int($r, (sp_int)($e0))", BOP_K(TY_INT) },
  { TY_COMPLEX, "/",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_div_poly($r, $b0)", BOP_K(TY_POLY) },
  { TY_COMPLEX, "/",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_div($r, $c0)", CX_NUM },
  { TY_COMPLEX, "/",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "/",   0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  { TY_COMPLEX, "+",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_add($r, $c0)", CX_NUM },
  { TY_COMPLEX, "+",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "+",   0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  { TY_COMPLEX, "-",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_sub($r, $c0)", CX_NUM },
  { TY_COMPLEX, "-",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "-",   0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  { TY_COMPLEX, "*",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_mul($r, $c0)", CX_NUM },
  { TY_COMPLEX, "*",   1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "*",   0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  { TY_COMPLEX, "quo", 1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_div($r, $c0)", CX_NUM },
  { TY_COMPLEX, "quo", 1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "quo", 0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  /* a whole-number Rational exponent stays exact (integer pow); a
     fractional one computes in floats (#2962) */
  { TY_COMPLEX, "**",  1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_pow($r, (sp_int)($e0))", BOP_K(TY_INT) },
  { TY_COMPLEX, "**",  1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_pow_c($r, $c0)", BOP_K(TY_FLOAT) | BOP_K(TY_COMPLEX) },
  { TY_COMPLEX, "**",  1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, "sp_complex_pow_rational($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_COMPLEX, "**",  1, 1, BF_ANY, TY_COMPLEX, BOPE_NONE, NULL, BOP_K(TY_POLY) },
  { TY_COMPLEX, "**",  1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "**",  0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  /* Complex has no modulo: NoMethodError, not a compile abort (#2618) */
  { TY_COMPLEX, "%",      1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE,
    "((void)($r), (void)($e0), (sp_raise_cls(\"NoMethodError\", \"undefined method '%' for an instance of Complex\"), (sp_Complex){0,0,0}))" },
  { TY_COMPLEX, "%",      0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  { TY_COMPLEX, "modulo", 1, 1, BF_ANY, TY_COMPLEX, BOPE_TEMPLATE,
    "((void)($r), (void)($e0), (sp_raise_cls(\"NoMethodError\", \"undefined method 'modulo' for an instance of Complex\"), (sp_Complex){0,0,0}))" },
  { TY_COMPLEX, "modulo", 0, 127, BF_ANY, TY_COMPLEX, BOPE_NONE },
  /* to_i/to_f/to_r require a zero imaginary part (RangeError otherwise);
     numerator/denominator model the Integer-component case (den 1) */
  { TY_COMPLEX, "to_i",        0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "sp_complex_to_int($r)" },
  { TY_COMPLEX, "to_i",        0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_COMPLEX, "to_int",      0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "sp_complex_to_int($r)" },
  { TY_COMPLEX, "to_int",      0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_COMPLEX, "to_f",        0,   0, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "sp_complex_to_f($r)" },
  { TY_COMPLEX, "to_f",        0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  { TY_COMPLEX, "to_r",        0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "sp_complex_to_r($r)" },
  { TY_COMPLEX, "to_r",        0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_COMPLEX, "<=>",         1,   1, BF_ANY, TY_INT,        BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_Complex _t$u = $c0; (_t$t.im == 0.0 && _t$u.im == 0.0)"
    " ? (_t$t.re < _t$u.re ? (sp_int)-1 : _t$t.re > _t$u.re ? (sp_int)1 : (sp_int)0)"
    " : SP_INT_NIL; })", BOP_K(TY_COMPLEX) | BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_COMPLEX, "<=>",         1,   1, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_COMPLEX, "zero?",       0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "({ sp_Complex _t$t = $r; (_t$t.re == 0.0 && _t$t.im == 0.0); })" },
  { TY_COMPLEX, "zero?",       0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "nonzero?",    0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; (_t$t.re != 0.0 || _t$t.im != 0.0) ? sp_box_complex(_t$t) : sp_box_nil(); })" },
  { TY_COMPLEX, "nonzero?",    0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_COMPLEX, "real?",       0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_COMPLEX, "real?",       0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "integer?",    0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_COMPLEX, "integer?",    0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "finite?",     0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "({ sp_Complex _t$t = $r; (isfinite(_t$t.re) && isfinite(_t$t.im)); })" },
  { TY_COMPLEX, "finite?",     0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "infinite?",   0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; (isinf(_t$t.re) || isinf(_t$t.im)) ? (sp_int)1 : SP_INT_NIL; })" },
  { TY_COMPLEX, "infinite?",   0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  /* eql? / equal? on the unboxed value: component equality against a
     Complex (eql? also compares the component classes; the struct has no
     object identity, so a self-reference compares equal, matching the
     common x.equal?(x) probe), constant false against anything else */
  { TY_COMPLEX, "eql?",        1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; sp_Complex _t$u = $e0; (_t$t.re == _t$u.re && _t$t.im == _t$u.im && _t$t.fl == _t$u.fl); })",
    BOP_K(TY_COMPLEX) },
  { TY_COMPLEX, "eql?",        1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_COMPLEX, "eql?",        0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "equal?",      1,   1, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "sp_complex_eq($r, $e0)", BOP_K(TY_COMPLEX) },
  { TY_COMPLEX, "equal?",      1,   1, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), 0)" },
  /* rationalize takes an optional eps argument (ignored -- a Complex with a
     zero imaginary part rationalizes its real part exactly) (#2556) */
  { TY_COMPLEX, "rationalize", 0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; if (_t$t.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Rational\"); sp_float_to_rational(_t$t.re); })" },
  { TY_COMPLEX, "rationalize", 1,   1, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE,
    "({ sp_Complex _t$t = $r; (void)($e0); if (_t$t.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Rational\"); sp_float_to_rational(_t$t.re); })" },
  { TY_COMPLEX, "numerator",   0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "$r" },
  { TY_COMPLEX, "numerator",   0, 127, BF_ANY, TY_COMPLEX,    BOPE_NONE },
  { TY_COMPLEX, "denominator", 0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "((void)($r), (sp_int)1)" },
  { TY_COMPLEX, "denominator", 0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  /* fdiv by a Complex is ordinary complex division in floats (#2555);
     coerce/fdiv against a non-numeric operand raise TypeError, not
     NoMethodError (#2964) */
  { TY_COMPLEX, "fdiv",        1,   1, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "sp_complex_div_real($r, (sp_float)($e0))", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_COMPLEX, "fdiv",        1,   1, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "sp_complex_div($r, $e0)", BOP_K(TY_COMPLEX) },
  { TY_COMPLEX, "fdiv",        1,   1, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, CX_TYPEERROR },
  { TY_COMPLEX, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t);"
    " sp_PolyArray_push(_t$t, sp_box_complex($c0)); sp_PolyArray_push(_t$t, sp_box_complex($r)); _t$t; })",
    BOP_K(TY_INT) | BOP_K(TY_FLOAT) | BOP_K(TY_COMPLEX) | BOP_K(TY_RATIONAL) },
  { TY_COMPLEX, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ (void)($r); (void)($e0); sp_raise_cls(\"TypeError\", \"can't be coerced into Complex\"); sp_PolyArray_new(); })" },
  /* == against a non-numeric value is always false (!= true); the operand
     still evaluates for its side effects (#2557) */
  { TY_COMPLEX, "==",          1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "(sp_complex_eq($r, $c0))", CX_NUM },
  { TY_COMPLEX, "==",          1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (void)($b0), 0)" },
  { TY_COMPLEX, "==",          0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_COMPLEX, "!=",          1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "(!sp_complex_eq($r, $c0))", CX_NUM },
  { TY_COMPLEX, "!=",          1,   1, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (void)($b0), 1)" },
  { TY_COMPLEX, "!=",          0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
#undef CX_NUM
#undef CX_TYPEERROR

  /* Rational (sp_Rational, by value: num and den). round/truncate/floor/
     ceil with a digit count, step and the operators are typed by their
     arguments in infer_numeric_call, so their rows leave inference to it
     (TY_UNKNOWN). Rational#i holds two floats where CRuby keeps the exact
     Rational (#2706). The operators against a Float are mostly claimed
     first by the Float <op> Rational arm, which sits above the lookup. */
#define RAT_IR  (BOP_K(TY_RATIONAL) | BOP_K(TY_INT))
  { TY_RATIONAL, "numerator",   0, 127, BF_ANY, TY_INT,        BOPE_TEMPLATE, "($r).num" },
  { TY_RATIONAL, "denominator", 0, 127, BF_ANY, TY_INT,        BOPE_TEMPLATE, "($r).den" },
  { TY_RATIONAL, "to_s",        0, 127, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_rational_to_s($r)" },
  { TY_RATIONAL, "inspect",     0, 127, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_rational_inspect($r)" },
  { TY_RATIONAL, "to_f",        0,   0, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "sp_rational_to_f($r)" },
  { TY_RATIONAL, "to_f",        0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  { TY_RATIONAL, "to_r",        0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  { TY_RATIONAL, "to_r",        0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "rationalize", 0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  /* rationalize(eps): the simplest rational within eps of self, by the
     Float path (it builds the [self-eps, self+eps] interval) (#3057) */
  { TY_RATIONAL, "rationalize", 1,   1, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE,
    "sp_float_rationalize(sp_rational_to_f($r), sp_rational_to_f($e0))", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "rationalize", 1,   1, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "sp_float_rationalize(sp_rational_to_f($r), $f0)" },
  { TY_RATIONAL, "rationalize", 0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "to_i",        0, 127, BF_ANY, TY_INT,        BOPE_TEMPLATE, "(($r).num / ($R).den)" },
  { TY_RATIONAL, "to_int",      0, 127, BF_ANY, TY_INT,        BOPE_TEMPLATE, "(($r).num / ($R).den)" },
  { TY_RATIONAL, "truncate",    0,   0, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "(($r).num / ($R).den)" },
  { TY_RATIONAL, "round",       0,   0, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "sp_rational_round_i($r)" },
  { TY_RATIONAL, "floor",       0,   0, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "sp_rational_floor_i($r)" },
  { TY_RATIONAL, "ceil",        0,   0, BF_ANY, TY_UNKNOWN,    BOPE_TEMPLATE, "sp_rational_ceil_i($r)" },
  { TY_RATIONAL, "round",       1,   2, BF_ANY, TY_UNKNOWN,    BOPE_RATIONAL_ROUND },
  { TY_RATIONAL, "truncate",    1,   2, BF_ANY, TY_UNKNOWN,    BOPE_RATIONAL_ROUND },
  { TY_RATIONAL, "floor",       1,   2, BF_ANY, TY_UNKNOWN,    BOPE_RATIONAL_ROUND },
  { TY_RATIONAL, "ceil",        1,   2, BF_ANY, TY_UNKNOWN,    BOPE_RATIONAL_ROUND },
  { TY_RATIONAL, "zero?",       0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "(($r).num == 0)" },
  { TY_RATIONAL, "zero?",       0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_RATIONAL, "positive?",   0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "(($r).num > 0)" },
  { TY_RATIONAL, "positive?",   0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_RATIONAL, "negative?",   0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "(($r).num < 0)" },
  { TY_RATIONAL, "negative?",   0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  /* Numeric predicates: a Rational is a finite, non-Integer real (#2562) */
  { TY_RATIONAL, "finite?",     0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_RATIONAL, "finite?",     0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_RATIONAL, "real?",       0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_RATIONAL, "real?",       0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_RATIONAL, "integer?",    0,   0, BF_ANY, TY_BOOL,       BOPE_TEMPLATE, "((void)($r), FALSE)" },
  { TY_RATIONAL, "integer?",    0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_RATIONAL, "infinite?",   0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "((void)($r), SP_INT_NIL)" },
  { TY_RATIONAL, "infinite?",   0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_RATIONAL, "nonzero?",    0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; _t$t.num != 0 ? sp_box_rational(_t$t) : sp_box_nil(); })" },
  { TY_RATIONAL, "nonzero?",    0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  /* Complex/real-projection methods on a real Rational (#2561) */
  { TY_RATIONAL, "real",        0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  { TY_RATIONAL, "real",        0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "conjugate",   0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  { TY_RATIONAL, "conjugate",   0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "conj",        0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  { TY_RATIONAL, "conj",        0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "imaginary",   0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "((void)($r), (sp_int)0)" },
  { TY_RATIONAL, "imaginary",   0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_RATIONAL, "imag",        0,   0, BF_ANY, TY_INT,        BOPE_TEMPLATE, "((void)($r), (sp_int)0)" },
  { TY_RATIONAL, "imag",        0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_RATIONAL, "arg",         0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "(($r).num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_RATIONAL, "arg",         0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_RATIONAL, "angle",       0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "(($r).num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_RATIONAL, "angle",       0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_RATIONAL, "phase",       0,   0, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "(($r).num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_RATIONAL, "phase",       0, 127, BF_ANY, TY_POLY,       BOPE_NONE },
  { TY_RATIONAL, "abs2",        0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "({ sp_Rational _t$t = $r; sp_rational_mul(_t$t, _t$t); })" },
  { TY_RATIONAL, "abs2",        0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "magnitude",   0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "sp_rational_abs($r)" },
  { TY_RATIONAL, "magnitude",   0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "to_c",        0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "((sp_Complex){sp_rational_to_f($r), 0, 1})" },
  { TY_RATIONAL, "to_c",        0, 127, BF_ANY, TY_COMPLEX,    BOPE_NONE },
  { TY_RATIONAL, "i",           0,   0, BF_ANY, TY_COMPLEX,    BOPE_TEMPLATE, "((sp_Complex){0.0, sp_rational_to_f($r), 2})" },
  { TY_RATIONAL, "rectangular", 0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new(); SP_GC_ROOT(_t$u);"
    " sp_PolyArray_push(_t$u, sp_box_rational(_t$t)); sp_PolyArray_push(_t$u, sp_box_int(0)); _t$u; })" },
  { TY_RATIONAL, "rectangular", 0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_RATIONAL, "rect",        0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new(); SP_GC_ROOT(_t$u);"
    " sp_PolyArray_push(_t$u, sp_box_rational(_t$t)); sp_PolyArray_push(_t$u, sp_box_int(0)); _t$u; })" },
  { TY_RATIONAL, "rect",        0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_RATIONAL, "polar",       0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_PolyArray *_t$u = sp_PolyArray_new(); SP_GC_ROOT(_t$u);"
    " sp_PolyArray_push(_t$u, sp_box_rational(sp_rational_abs(_t$t)));"
    " sp_PolyArray_push(_t$u, _t$t.num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0));"
    " _t$u; })" },
  { TY_RATIONAL, "polar",       0, 127, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  /* coerce(n): [n as Rational, self]; against a Float both convert to
     Float (#2568) */
  { TY_RATIONAL, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t);"
    " sp_PolyArray_push(_t$t, sp_box_rational(sp_rational_new($e0, 1)));"
    " sp_PolyArray_push(_t$t, sp_box_rational($r)); _t$t; })", BOP_K(TY_INT) },
  { TY_RATIONAL, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t);"
    " sp_PolyArray_push(_t$t, sp_box_rational($e0));"
    " sp_PolyArray_push(_t$t, sp_box_rational($r)); _t$t; })", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE,
    "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t);"
    " sp_PolyArray_push(_t$t, sp_box_float($e0));"
    " sp_PolyArray_push(_t$t, sp_box_float(sp_rational_to_f($r))); _t$t; })", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "coerce",      1,   1, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  /* % / modulo / remainder / divmod: exact against a Rational or Integer,
     in floats against a Float ([Integer quotient, Float remainder] for
     divmod, #2595) */
  { TY_RATIONAL, "%",         1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_mod($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "%",         1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_mod($r, sp_rational_new($e0, 1))", BOP_K(TY_INT) },
  { TY_RATIONAL, "%",         1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_fmod(sp_rational_to_f($r), $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "modulo",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_mod($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "modulo",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_mod($r, sp_rational_new($e0, 1))", BOP_K(TY_INT) },
  { TY_RATIONAL, "modulo",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_fmod(sp_rational_to_f($r), $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "remainder", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_rem($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "remainder", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_rem($r, sp_rational_new($e0, 1))", BOP_K(TY_INT) },
  { TY_RATIONAL, "remainder", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "fmod(sp_rational_to_f($r), $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "divmod",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_Rational _t$u = $e0; sp_int _t$v = sp_rational_idiv(_t$t, _t$u);"
    " sp_PolyArray *_t$w = sp_PolyArray_new(); SP_GC_ROOT(_t$w);"
    " sp_PolyArray_push(_t$w, sp_box_int(_t$v));"
    " sp_PolyArray_push(_t$w, sp_box_rational(sp_rational_mod(_t$t, _t$u))); _t$w; })", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "divmod",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_Rational _t$u = sp_rational_new($e0, 1); sp_int _t$v = sp_rational_idiv(_t$t, _t$u);"
    " sp_PolyArray *_t$w = sp_PolyArray_new(); SP_GC_ROOT(_t$w);"
    " sp_PolyArray_push(_t$w, sp_box_int(_t$v));"
    " sp_PolyArray_push(_t$w, sp_box_rational(sp_rational_mod(_t$t, _t$u))); _t$w; })", BOP_K(TY_INT) },
  { TY_RATIONAL, "divmod",    1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_float _t$t = sp_rational_to_f($r); sp_float _t$u = $f0; sp_int _t$v = (sp_int)floor(_t$t / _t$u);"
    " sp_PolyArray *_t$w = sp_PolyArray_new(); SP_GC_ROOT(_t$w);"
    " sp_PolyArray_push(_t$w, sp_box_int(_t$v));"
    " sp_PolyArray_push(_t$w, sp_box_float(_t$t - (sp_float)_t$v * _t$u)); _t$w; })", BOP_K(TY_FLOAT) },
  /* ** computes in floats except against an Integer (CRuby; a negative
     base with a fractional exponent would be Complex, out of the value
     model -- it yields NaN here) */
  { TY_RATIONAL, "**",        1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "pow(sp_rational_to_f($r), sp_rational_to_f($e0))", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "**",        1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_pow($r, (sp_int)($e0))", BOP_K(TY_INT) },
  { TY_RATIONAL, "**",        1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "pow(sp_rational_to_f($r), $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "-@",          0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "sp_rational_neg($r)" },
  { TY_RATIONAL, "-@",          0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "+@",          0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "$r" },
  { TY_RATIONAL, "+@",          0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  { TY_RATIONAL, "abs",         0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "sp_rational_abs($r)" },
  { TY_RATIONAL, "abs",         0, 127, BF_ANY, TY_RATIONAL,   BOPE_NONE },
  /* arithmetic: against a Complex in floats (see emit_complex_coerce);
     against a Rational or an Integer exact; against a Float self converts
     to Float. A poly operand (a Rational out of a poly array, say) is not
     modeled and falls through to the generic path. */
  { TY_RATIONAL, "+",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_complex_add(((sp_Complex){sp_rational_to_f($r), 0, 1}), $e0)", BOP_K(TY_COMPLEX) },
  { TY_RATIONAL, "+",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) + $e0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "+",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_add($r, $q0)", RAT_IR },
  { TY_RATIONAL, "-",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_complex_sub(((sp_Complex){sp_rational_to_f($r), 0, 1}), $e0)", BOP_K(TY_COMPLEX) },
  { TY_RATIONAL, "-",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) - $e0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "-",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_sub($r, $q0)", RAT_IR },
  { TY_RATIONAL, "*",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_complex_mul(((sp_Complex){sp_rational_to_f($r), 0, 1}), $e0)", BOP_K(TY_COMPLEX) },
  { TY_RATIONAL, "*",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) * $e0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "*",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_mul($r, $q0)", RAT_IR },
  { TY_RATIONAL, "/",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_complex_div(((sp_Complex){sp_rational_to_f($r), 0, 1}), $e0)", BOP_K(TY_COMPLEX) },
  { TY_RATIONAL, "/",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) / $e0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "/",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_div($r, $q0)", RAT_IR },
  { TY_RATIONAL, "quo", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) / $e0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "quo", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_div($r, $q0)", RAT_IR },
  /* fdiv: float division whatever the operand; div: floor division to an
     Integer (Numeric#div) */
  { TY_RATIONAL, "fdiv",        1,   1, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "(sp_rational_to_f($r) / sp_rational_to_f($e0))", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "fdiv",        1,   1, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "(sp_rational_to_f($r) / $f0)", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "fdiv",        0, 127, BF_ANY, TY_FLOAT,      BOPE_NONE },
  { TY_RATIONAL, "div",         1,   1, BF_ANY, TY_INT,        BOPE_TEMPLATE, "((sp_int)floor(sp_rational_to_f($r) / ($e0)))", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "div",         1,   1, BF_ANY, TY_INT,        BOPE_TEMPLATE, "sp_rational_idiv($r, $q0)", RAT_IR },
  { TY_RATIONAL, "div",         0, 127, BF_ANY, TY_INT,        BOPE_NONE },
  /* comparisons: against a Float by float value (coercing the Float to a
     Rational truncates it, 1.5 -> 1/1, and compares wrong) */
  { TY_RATIONAL, "<",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) < $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "<",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_cmp($r, $q0) < 0)", RAT_IR },
  { TY_RATIONAL, ">",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) > $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, ">",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_cmp($r, $q0) > 0)", RAT_IR },
  { TY_RATIONAL, "<=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) <= $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "<=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_cmp($r, $q0) <= 0)", RAT_IR },
  { TY_RATIONAL, ">=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) >= $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, ">=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_cmp($r, $q0) >= 0)", RAT_IR },
  { TY_RATIONAL, "<=>", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_float _t$t = sp_rational_to_f($r); sp_float _t$u = $f0; _t$t < _t$u ? -1 : (_t$t > _t$u ? 1 : 0); })", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "<=>", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_cmp($r, $q0)", RAT_IR },
  /* == / != / === (=== is value equality for a Numeric, #2564). A numeric
     operand compares by value. A poly one is not a non-numeric one: at run
     time it is very often the Rational that came out of an Array, so the
     runtime compares (#3382). Anything else is never == (#2572). */
  { TY_RATIONAL, "==",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) == $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "==",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_eq($r, $q0))", RAT_IR },
  { TY_RATIONAL, "==",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_poly_eq(sp_box_rational($r), $b0))", BOP_K(TY_POLY) | BOP_K(TY_UNKNOWN) },
  { TY_RATIONAL, "==",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "((void)($r), (void)($b0), 0)" },
  { TY_RATIONAL, "===", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) == $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "===", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_eq($r, $q0))", RAT_IR },
  { TY_RATIONAL, "===", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_poly_eq(sp_box_rational($r), $b0))", BOP_K(TY_POLY) | BOP_K(TY_UNKNOWN) },
  { TY_RATIONAL, "===", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "((void)($r), (void)($b0), 0)" },
  { TY_RATIONAL, "!=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(sp_rational_to_f($r) != $f0)", BOP_K(TY_FLOAT) },
  { TY_RATIONAL, "!=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(!sp_rational_eq($r, $q0))", RAT_IR },
  { TY_RATIONAL, "!=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "(!sp_poly_eq(sp_box_rational($r), $b0))", BOP_K(TY_POLY) | BOP_K(TY_UNKNOWN) },
  { TY_RATIONAL, "!=",  1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "((void)($r), (void)($b0), 1)" },
  /* Comparable#between? / #clamp via <=> (#2563): clamp between two
     Rationals answers the receiver or a bound; with another bound the
     applied bound keeps its own class (#3233) */
  { TY_RATIONAL, "between?", 2, 2, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; (sp_rational_cmp(_t$t, $q0) >= 0 && sp_rational_cmp(_t$t, $q1) <= 0); })", RAT_IR, RAT_IR },
  { TY_RATIONAL, "clamp",    2, 2, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_Rational _t$t = $r; sp_Rational _t$u = $e0; sp_Rational _t$v = $e1;"
    " sp_rational_cmp(_t$t, _t$u) < 0 ? _t$u : (sp_rational_cmp(_t$t, _t$v) > 0 ? _t$v : _t$t); })",
    BOP_K(TY_RATIONAL), BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "clamp",    2, 2, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_num_clamp(sp_box_rational($r), $b0, $b1)" },
  /* eql? / equal? on the unboxed value: component equality against a
     Rational; eql? asks a poly operand at run time whether it IS a
     Rational (Rational(1,1).eql?(1) is false, #3382); constant false
     otherwise */
  { TY_RATIONAL, "eql?",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_eq($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "eql?",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE,
    "({ sp_RbVal _t$t = $b0; sp_poly_is_rational(_t$t) && sp_poly_eq(sp_box_rational($r), _t$t); })",
    BOP_K(TY_POLY) | BOP_K(TY_UNKNOWN) },
  { TY_RATIONAL, "eql?",   1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_RATIONAL, "equal?", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "sp_rational_eq($r, $e0)", BOP_K(TY_RATIONAL) },
  { TY_RATIONAL, "equal?", 1, 1, BF_ANY, TY_UNKNOWN, BOPE_TEMPLATE, "((void)($r), 0)" },
#undef RAT_IR

  /* String range ("a".."e"): the endpoints answer natively (with a count,
     the materialized prefix); every traversal rides the element array
     (#3064). step(n) / %(n) is an Enumerator over every nth member (#3671).
     #size counts INTEGER elements, so it is nil here, as in CRuby. */
  { TY_STR_RANGE, "begin",        0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; _t$T.first; })" },
  { TY_STR_RANGE, "end",          0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; _t$T.last; })" },
  /* nil for an empty range, a raise for an open side, the members walked for an excluded end */
  { TY_STR_RANGE, "min",          0,   0, BF_NONE,     TY_STRING,      BOPE_TEMPLATE, "sp_srange_min_v($r)" },
  { TY_STR_RANGE, "max",          0,   0, BF_NONE,     TY_STRING,      BOPE_TEMPLATE, "sp_srange_max_v($r)" },
  /* [min, max] off the endpoints, max first as CRuby's range_minmax evaluates them */
  { TY_STR_RANGE, "minmax",       0,   0, BF_NONE,     TY_STR_ARRAY,   BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; SP_GC_ROOT_STR(_t$T.first); SP_GC_ROOT_STR(_t$T.last); const char *_t$t = sp_srange_max_v(_t$T); SP_GC_ROOT_STR(_t$t); const char *_t$u = sp_srange_min_v(_t$T); SP_GC_ROOT_STR(_t$u); sp_StrArray *_r$T = sp_StrArray_new(); sp_StrArray_push(_r$T, _t$u); sp_StrArray_push(_r$T, _t$t); _r$T; })" },
  { TY_STR_RANGE, "to_s",         0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_srange_to_s(_t$T); })" },
  { TY_STR_RANGE, "inspect",      0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_srange_inspect(_t$T); })" },
  { TY_STR_RANGE, "first",        0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; _t$T.first; })" },
  { TY_STR_RANGE, "last",         0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; _t$T.last; })" },
  { TY_STR_RANGE, "min",          1,   1, BF_NONE, TY_STR_ARRAY,   BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_StrArray *_t$t = sp_srange_to_a(_t$T); SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_StrArray_slice(_t$t, 0, _t$u); })", 0 },  /* the n smallest or largest members (#3665) */
  { TY_STR_RANGE, "max",          1,   1, BF_NONE, TY_STR_ARRAY,   BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_StrArray *_t$t = sp_srange_to_a(_t$T); SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_StrArray_reverse_bang(_t$t); sp_StrArray_slice(_t$t, 0, _t$u); })", 0 },
  { TY_STR_RANGE, "exclude_end?", 0,   0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; (sp_bool)_t$T.excl; })", 0 },
  { TY_STR_RANGE, "class",        0,   0, BF_ANY,  TY_CLASS,       BOPE_TEMPLATE, "((void)($r), ((sp_Class){0, SPL(\"Range\")}))", 0 },
  { TY_STR_RANGE, "frozen?",      0,   0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($r), (sp_bool)1)", 0 },
  { TY_STR_RANGE, "freeze",       0,   0, BF_ANY,  TY_STR_RANGE,   BOPE_TEMPLATE, "$r", 0 },
  { TY_STR_RANGE, "itself",       0,   0, BF_ANY,  TY_STR_RANGE,   BOPE_TEMPLATE, "$r", 0 },
  { TY_STR_RANGE, "dup",          0,   0, BF_ANY,  TY_STR_RANGE,   BOPE_TEMPLATE, "$r", 0 },
  { TY_STR_RANGE, "clone",        0,   0, BF_ANY,  TY_STR_RANGE,   BOPE_TEMPLATE, "$r", 0 },
  { TY_STR_RANGE, "begin",        1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "end",          1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "min",          1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "max",          1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "to_s",         1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "inspect",      1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "first",        1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "last",         1, 127, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "cover?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "include?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "member?",      0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "===",          0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "==",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "!=",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "eql?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "exclude_end?", 0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "frozen?",      0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "nil?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "is_a?",        0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "kind_of?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "instance_of?", 0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "equal?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "respond_to?",  0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_STR_RANGE, "step",         0,   1, BF_REQUIRED, TY_STR_RANGE,   BOPE_NONE },  /* with a block: the receiver, as CRuby */
  { TY_STR_RANGE, "step",         1,   1, BF_NONE,     TY_ENUMERATOR,  BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_StrArray *_t$t = sp_srange_to_a(_t$T); SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u <= 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\"); sp_StrArray *_t$v = sp_StrArray_new(); SP_GC_ROOT(_t$v); for (sp_int _t$w = 0; _t$w < sp_StrArray_length(_t$t); _t$w += _t$u) sp_StrArray_push(_t$v, sp_StrArray_get(_t$t, _t$w)); sp_Enumerator_new_from(sp_box_str_array(_t$v)); })" },
  { TY_STR_RANGE, "%",            1,   1, BF_NONE,     TY_ENUMERATOR,  BOPE_TEMPLATE, "({ sp_StrRange _t$T = $r; sp_StrArray *_t$t = sp_srange_to_a(_t$T); SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u <= 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\"); sp_StrArray *_t$v = sp_StrArray_new(); SP_GC_ROOT(_t$v); for (sp_int _t$w = 0; _t$w < sp_StrArray_length(_t$t); _t$w += _t$u) sp_StrArray_push(_t$v, sp_StrArray_get(_t$t, _t$w)); sp_Enumerator_new_from(sp_box_str_array(_t$v)); })" },
  { TY_STR_RANGE, "class",        0, 127, BF_ANY,      TY_CLASS,       BOPE_NONE },
  { TY_STR_RANGE, "hash",         0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_STR_RANGE, "size",         0,   0, BF_ANY,      TY_NIL,         BOPE_TEMPLATE, "((void)($r), SP_INT_NIL)" },
  { TY_STR_RANGE, "to_a",         0,   0, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "entries",      0,   0, BF_ANY,      TY_STR_ARRAY,   BOPE_NONE },
  { TY_STR_RANGE, "freeze",       0, 127, BF_ANY,      TY_STR_RANGE,   BOPE_NONE },
  { TY_STR_RANGE, "itself",       0, 127, BF_ANY,      TY_STR_RANGE,   BOPE_NONE },
  { TY_STR_RANGE, "dup",          0, 127, BF_ANY,      TY_STR_RANGE,   BOPE_NONE },
  { TY_STR_RANGE, "clone",        0, 127, BF_ANY,      TY_STR_RANGE,   BOPE_NONE },

  /* Float range (1.0..3.0): not iterable. minmax is the endpoints (#3690).
     The iterators raise "can't iterate from Float" at run time and type
     poly, which keeps the raise's boxed-nil slot valid and lets respond_to?
     report them present, as CRuby does. The endpoint readers and #size
     read the literal and stay in infer_range_call. */
  { TY_FLOAT_RANGE, "begin",        0,   0, BF_ANY,  TY_UNKNOWN,     BOPE_TEMPLATE, "sp_frange_begin_v($r)", 0 },  /* the endpoint the literal wrote is typed in infer_range_call */
  { TY_FLOAT_RANGE, "first",        0,   0, BF_ANY,  TY_UNKNOWN,     BOPE_TEMPLATE, "sp_frange_first_v($r)", 0 },  /* the endpoint the literal wrote is typed in infer_range_call */
  { TY_FLOAT_RANGE, "min",          0,   0, BF_ANY,  TY_UNKNOWN,     BOPE_TEMPLATE, "sp_frange_min_v($r)", 0 },  /* the endpoint the literal wrote is typed in infer_range_call */
  { TY_FLOAT_RANGE, "min",          1,   1, BF_NONE, TY_UNKNOWN,     BOPE_TEMPLATE, "({ sp_frange_minn_raise($r, $i0); sp_box_nil(); })", 0 },  /* min(n)/max(n) enumerate (#3665) */
  { TY_FLOAT_RANGE, "max",          1,   1, BF_NONE, TY_UNKNOWN,     BOPE_TEMPLATE, "({ sp_frange_maxn_raise($r, $i0); sp_box_nil(); })", 0 },  /* min(n)/max(n) enumerate (#3665) */
  { TY_FLOAT_RANGE, "minmax",       0,   0, BF_NONE, TY_FLOAT_ARRAY, BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_float _t$t = sp_frange_max_v(_t$T); sp_float _t$u = sp_frange_min_v(_t$T); sp_FloatArray *_r$T = sp_FloatArray_new(); SP_GC_ROOT(_r$T); sp_FloatArray_push_nilable(_r$T, _t$u); sp_FloatArray_push_nilable(_r$T, _t$t); _r$T; })", 0 },  /* the endpoints (#3690): max first, as CRuby's range_minmax evaluates them, and nil for an empty range */
  { TY_FLOAT_RANGE, "cover?",       1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_cover(_t$T, $f0); })", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_FLOAT_RANGE, "cover?",       1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_RbVal _a$T = $b0; sp_frange_cover_poly(_t$T, _a$T); })", BOP_K(TY_POLY) | BOP_K(TY_RATIONAL) | BOP_K(TY_BIGINT) },
  { TY_FLOAT_RANGE, "cover?",       1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },  /* never covers a non-number */
  { TY_FLOAT_RANGE, "include?",     1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_cover(_t$T, $f0); })", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_FLOAT_RANGE, "include?",     1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_RbVal _a$T = $b0; sp_frange_cover_poly(_t$T, _a$T); })", BOP_K(TY_POLY) | BOP_K(TY_RATIONAL) | BOP_K(TY_BIGINT) },
  { TY_FLOAT_RANGE, "include?",     1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },  /* never covers a non-number */
  { TY_FLOAT_RANGE, "member?",      1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_cover(_t$T, $f0); })", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_FLOAT_RANGE, "member?",      1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_RbVal _a$T = $b0; sp_frange_cover_poly(_t$T, _a$T); })", BOP_K(TY_POLY) | BOP_K(TY_RATIONAL) | BOP_K(TY_BIGINT) },
  { TY_FLOAT_RANGE, "member?",      1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },  /* never covers a non-number */
  { TY_FLOAT_RANGE, "===",          1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_cover(_t$T, $f0); })", BOP_K(TY_INT) | BOP_K(TY_FLOAT) },
  { TY_FLOAT_RANGE, "===",          1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_RbVal _a$T = $b0; sp_frange_cover_poly(_t$T, _a$T); })", BOP_K(TY_POLY) | BOP_K(TY_RATIONAL) | BOP_K(TY_BIGINT) },
  { TY_FLOAT_RANGE, "===",          1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },  /* never covers a non-number */
  { TY_FLOAT_RANGE, "exclude_end?", 0,   0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; (sp_bool)_t$T.excl; })", 0 },
  { TY_FLOAT_RANGE, "==",           1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_FloatRange _t$t = $e0; sp_frange_eq(_t$T, _t$t); })", BOP_K(TY_FLOAT_RANGE) },
  { TY_FLOAT_RANGE, "==",           1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },
  { TY_FLOAT_RANGE, "eql?",         1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_FloatRange _t$t = $e0; sp_frange_eq(_t$T, _t$t); })", BOP_K(TY_FLOAT_RANGE) },
  { TY_FLOAT_RANGE, "eql?",         1,   1, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($e0), 0)", 0 },
  { TY_FLOAT_RANGE, "to_s",         0,   0, BF_ANY,  TY_STRING,      BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_inspect(_t$T); })", 0 },
  { TY_FLOAT_RANGE, "inspect",      0,   0, BF_ANY,  TY_STRING,      BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_frange_inspect(_t$T); })", 0 },
  { TY_FLOAT_RANGE, "step",         1,   1, BF_NONE, TY_FLOAT_ARRAY, BOPE_TEMPLATE, "({ sp_FloatRange _t$T = $r; sp_FloatArray_from_step(_t$T.first, _t$T.last, $f0, _t$T.excl); })", 0 },
  { TY_FLOAT_RANGE, "class",        0,   0, BF_ANY,  TY_CLASS,       BOPE_TEMPLATE, "((void)($r), ((sp_Class){0, SPL(\"Range\")}))", 0 },
  { TY_FLOAT_RANGE, "frozen?",      0,   0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($r), (sp_bool)1)", 0 },
  { TY_FLOAT_RANGE, "freeze",       0,   0, BF_ANY,  TY_FLOAT_RANGE, BOPE_TEMPLATE, "$r", 0 },
  { TY_FLOAT_RANGE, "itself",       0,   0, BF_ANY,  TY_FLOAT_RANGE, BOPE_TEMPLATE, "$r", 0 },
  { TY_FLOAT_RANGE, "dup",          0,   0, BF_ANY,  TY_FLOAT_RANGE, BOPE_TEMPLATE, "$r", 0 },
  { TY_FLOAT_RANGE, "clone",        0,   0, BF_ANY,  TY_FLOAT_RANGE, BOPE_TEMPLATE, "$r", 0 },
  { TY_FLOAT_RANGE, "nil?",         0,   0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($r), (sp_bool)0)", 0 },  /* a Range value is never nil */
  { TY_FLOAT_RANGE, "first",        0, 127, BF_ANY,  TY_UNKNOWN,     BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })", 0 },  /* first(n)/last(n) iterate */
  { TY_FLOAT_RANGE, "last",         0, 127, BF_ANY,  TY_UNKNOWN,     BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })", 0 },  /* first(n)/last(n) iterate */
  { TY_FLOAT_RANGE, "cover?",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "include?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "member?",          0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "===",              0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "==",               0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "!=",               0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "eql?",             0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "exclude_end?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "frozen?",          0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "nil?",             0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "is_a?",            0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "kind_of?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "instance_of?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "equal?",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "respond_to?",      0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_FLOAT_RANGE, "to_s",             0, 127, BF_ANY,      TY_STRING,      BOPE_NONE },
  { TY_FLOAT_RANGE, "inspect",          0, 127, BF_ANY,      TY_STRING,      BOPE_NONE },
  { TY_FLOAT_RANGE, "minmax",           0,   0, BF_ANY,      TY_FLOAT_ARRAY, BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "step",             0, 127, BF_REQUIRED, TY_FLOAT_RANGE, BOPE_NONE },  /* with a block: the receiver, as CRuby */
  { TY_FLOAT_RANGE, "step",             0, 127, BF_ANY,      TY_FLOAT_ARRAY, BOPE_NONE },
  { TY_FLOAT_RANGE, "bsearch",          0, 127, BF_REQUIRED, TY_FLOAT,       BOPE_NONE },
  { TY_FLOAT_RANGE, "class",            0, 127, BF_ANY,      TY_CLASS,       BOPE_NONE },
  { TY_FLOAT_RANGE, "freeze",           0, 127, BF_ANY,      TY_FLOAT_RANGE, BOPE_NONE },
  { TY_FLOAT_RANGE, "itself",           0, 127, BF_ANY,      TY_FLOAT_RANGE, BOPE_NONE },
  { TY_FLOAT_RANGE, "dup",              0, 127, BF_ANY,      TY_FLOAT_RANGE, BOPE_NONE },
  { TY_FLOAT_RANGE, "clone",            0, 127, BF_ANY,      TY_FLOAT_RANGE, BOPE_NONE },
  { TY_FLOAT_RANGE, "each",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "map",              0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "collect",          0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "select",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "filter",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "reject",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "to_a",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 1); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "to_h",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "entries",          0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 1); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "find",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "detect",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "find_index",       0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "count",            0,   0, BF_NONE,     TY_POLY,        BOPE_TEMPLATE, "sp_frange_count_v($r)" },  /* an open one counts Infinity */
  { TY_FLOAT_RANGE, "count",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "sum",              0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "sort",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "sort_by",          0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "min_by",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "max_by",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "reduce",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "inject",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "each_with_index",  0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "flat_map",         0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "collect_concat",   0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "any?",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "all?",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "none?",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "one?",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "take",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "drop",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "take_while",       0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "drop_while",       0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "filter_map",       0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "partition",        0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "group_by",         0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "each_with_object", 0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "tally",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "find_all",         0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "zip",              0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "grep",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "grep_v",           0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "uniq",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "reverse",          0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "minmax",           1, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "join",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "index",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "size",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "lazy",             0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "each_cons",        0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "each_slice",       0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "chunk",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "chunk_while",      0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },
  { TY_FLOAT_RANGE, "cycle",            0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "({ sp_frange_iter_raise($r, 0); sp_box_nil(); })" },

  /* Enumerator. The block forms run over the materialized pairs or the
     lazy #next driver. Enumerator#+ and with_index with a block are typed
     by their operand and receiver in infer_call_inner. */
  { TY_ENUMERATOR, "next",            0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "sp_Enumerator_next($r)", 0 },  /* StopIteration past the end */
  { TY_ENUMERATOR, "next",            0, 127, BF_ANY,      TY_POLY,        BOPE_NONE },
  { TY_ENUMERATOR, "peek",            0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "sp_Enumerator_peek($r)", 0 },
  { TY_ENUMERATOR, "peek",            0, 127, BF_ANY,      TY_POLY,        BOPE_NONE },
  { TY_ENUMERATOR, "find",            0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },  /* lazily via #next, nil on no match (#3236) */
  { TY_ENUMERATOR, "detect",          0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },  /* lazily via #next, nil on no match (#3236) */
  { TY_ENUMERATOR, "take_while",      0, 127, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },  /* the same lazy driver (#3590) */
  { TY_ENUMERATOR, "include?",        1,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "member?",         1,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "find_index",      1,   1, BF_NONE,     TY_INT,         BOPE_NONE },
  { TY_ENUMERATOR, "next_values",     0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "sp_Enumerator_next_values($r)", 0 },
  { TY_ENUMERATOR, "next_values",     0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },  /* #2482 */
  { TY_ENUMERATOR, "peek_values",     0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "sp_Enumerator_peek_values($r)", 0 },
  { TY_ENUMERATOR, "peek_values",     0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },  /* #2482 */
  { TY_ENUMERATOR, "rewind",          0,   0, BF_ANY,      TY_ENUMERATOR,  BOPE_TEMPLATE, "sp_Enumerator_rewind($r)", 0 },
  { TY_ENUMERATOR, "rewind",          0, 127, BF_ANY,      TY_ENUMERATOR,  BOPE_NONE },
  { TY_ENUMERATOR, "frozen?",         0,   0, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r)->frozen)", 0 },
  { TY_ENUMERATOR, "frozen?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "equal?",          1,   1, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) == ($e0))", BOP_K(TY_ENUMERATOR) },
  { TY_ENUMERATOR, "equal?",          1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "eql?",            1,   1, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) == ($e0))", BOP_K(TY_ENUMERATOR) },
  { TY_ENUMERATOR, "eql?",            1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "==",              1,   1, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) == ($e0))", BOP_K(TY_ENUMERATOR) },
  { TY_ENUMERATOR, "==",              1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_ENUMERATOR, "freeze",          0,   0, BF_ANY,      TY_ENUMERATOR,  BOPE_TEMPLATE, "({ sp_Enumerator *_t$t = $r; _t$t->frozen = TRUE; _t$t; })", 0 },
  { TY_ENUMERATOR, "freeze",          0, 127, BF_ANY,      TY_ENUMERATOR,  BOPE_NONE },
  { TY_ENUMERATOR, "itself",          0,   0, BF_ANY,      TY_ENUMERATOR,  BOPE_TEMPLATE, "$r", 0 },
  { TY_ENUMERATOR, "itself",          0, 127, BF_ANY,      TY_ENUMERATOR,  BOPE_NONE },
  { TY_ENUMERATOR, "feed",            1,   1, BF_ANY,      TY_NIL,         BOPE_TEMPLATE, "sp_Enumerator_feed($r, $b0)" },
  { TY_ENUMERATOR, "with_index",      0,   1, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },  /* over [element, index] pairs */
  { TY_ENUMERATOR, "each_with_index", 0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },  /* #2487 */
  { TY_ENUMERATOR, "each_index",      0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },  /* #2487 */
  { TY_ENUMERATOR, "size",            0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "sp_Enumerator_size($r)", 0 },
  { TY_ENUMERATOR, "size",            0, 127, BF_ANY,      TY_POLY,        BOPE_NONE },  /* nil, an Integer or a stored size */
  { TY_ENUMERATOR, "take",            1,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "sp_Enumerator_take($r, $i0)" },
  { TY_ENUMERATOR, "first",           1,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "sp_Enumerator_take($r, $i0)" },
  { TY_ENUMERATOR, "drop",            1,   1, BF_NONE,     TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "reject",          0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "select",          0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "filter",          0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "map",             0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "collect",         0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "sort_by",         0,   0, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "sum",             0,   0, BF_REQUIRED, TY_POLY,        BOPE_NONE },
  { TY_ENUMERATOR, "to_a",            0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "entries",         0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },
  { TY_ENUMERATOR, "dup",             0,   0, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "sp_Enumerator_dup($r)", 0 },  /* typed by the generic rules */
  { TY_ENUMERATOR, "clone",           0,   0, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "sp_Enumerator_dup($r)", 0 },
  { TY_ENUMERATOR, "inspect",         0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_enum_inspect($r)" },
  { TY_ENUMERATOR, "to_s",            0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_enum_inspect($r)" },

  /* Time: the result kinds of the calls whose arms render more than the
     receiver (emitted in emit_value_recv_call). Time - Time and Time - poly
     are typed by the operand before the lookup; iso8601, httpdate and
     rfc2822 depend on the time feature, <=> on the operand. */
  { TY_TIME, "utc",        0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "gmtime",     0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "getutc",     0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "localtime",  0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "getlocal",   0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "+",         1,   1, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_add($r, (sp_float)($e0))", TY_UNKNOWN },
  { TY_TIME, "+",          0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "-",         1,   1, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_add($r, -(sp_float)($e0))", TY_UNKNOWN },
  { TY_TIME, "-",          0, 127, BF_ANY, TY_TIME,       BOPE_NONE },
  { TY_TIME, "clamp",      2,   2, BF_ANY, TY_TIME,       BOPE_NONE },  /* self or a bound */
  { TY_TIME, "to_a",       0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_TIME, "to_r",      0,   0, BF_ANY, TY_RATIONAL,   BOPE_TEMPLATE, "({ sp_Time _t$t = $r; sp_rational_new_i64((int64_t)_t$t.tv_sec * 1000000000LL + _t$t.tv_nsec, 1000000000); })", TY_UNKNOWN },
  { TY_TIME, "floor",     0,   0, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, 0, 0)", TY_UNKNOWN },  /* ndigits 0: whole seconds (#3089) */
  { TY_TIME, "floor",     1,   1, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, $i0, 0)", TY_UNKNOWN },  /* a negative count raises (#3700) */
  { TY_TIME, "ceil",      0,   0, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, 0, 1)", TY_UNKNOWN },
  { TY_TIME, "ceil",      1,   1, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, $i0, 1)", TY_UNKNOWN },
  { TY_TIME, "round",     0,   0, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, 0, 2)", TY_UNKNOWN },
  { TY_TIME, "round",     1,   1, BF_ANY, TY_TIME,       BOPE_TEMPLATE, "sp_time_round_to($r, $i0, 2)", TY_UNKNOWN },
  { TY_TIME, "xmlschema", 1,   1, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_time_iso8601_frac($r, $i0)", TY_UNKNOWN },  /* fraction digits (#3094) */
  { TY_TIME, "xmlschema", 0, 127, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_time_iso8601($r)", TY_UNKNOWN },
  { TY_TIME, "deconstruct_keys", 1,   1, BF_ANY, TY_POLY,       BOPE_NONE },  /* a boxed Symbol => Integer hash */
  { TY_TIME, "strftime",  1,   1, BF_ANY, TY_STRING,     BOPE_TEMPLATE, "sp_time_strftime($r, $s0)", TY_UNKNOWN },
  { TY_TIME, "strftime",   0, 127, BF_ANY, TY_STRING,     BOPE_NONE },
  { TY_TIME, "to_f",      0, 127, BF_ANY, TY_FLOAT,      BOPE_TEMPLATE, "({ sp_Time _t$t = ($r); sp_time_ns_to_f(_t$t.tv_sec, _t$t.tv_nsec); })", TY_UNKNOWN },  /* one read of the receiver (#2865) */
  { TY_TIME, "subsec",    0, 127, BF_ANY, TY_POLY,       BOPE_TEMPLATE, "({ sp_Time _t$t = $r; _t$t.tv_nsec == 0 ? sp_box_int(0) : sp_box_rational(sp_rational_new((sp_int)_t$t.tv_nsec, 1000000000)); })", TY_UNKNOWN },  /* Integer 0 for a whole second, else a Rational */
  { TY_TIME, "<",          0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_TIME, ">",          0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_TIME, "<=",         0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_TIME, ">=",         0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_TIME, "==",         0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_TIME, "!=",         0, 127, BF_ANY, TY_BOOL,       BOPE_NONE },
  /* The concurrency handles: Fiber, Thread, Queue (SizedQueue too: one
     kind, told apart at run time by its bound), Mutex and
     ConditionVariable. Inference typed most names for any arity where
     codegen emits only some, so a name often has a codegen row for its
     arity and a wider BOPE_NONE row after it that only types the call. */

  /* Fiber. #inspect / #to_s are emitted here but were never typed by the
     Fiber rules, so they leave inference to the rules after them. */
  { TY_FIBER, "resume",        0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_FIBER_RESUME },
  { TY_FIBER, "transfer",      0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_FIBER_TRANSFER },
  { TY_FIBER, "raise",         0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_FIBER_RAISE },
  /* the fiber's own storage by a literal key: what a `Fiber.attr_accessor`
     reader and writer (desugar_handle_attr_accessor) read and write on self */
  { TY_FIBER, "__storage_get", 1, 1, BF_ANY, TY_POLY, BOPE_TEMPLATE, "sp_Fiber_attr_get($r, $e0)", BOP_K(TY_SYMBOL) },
  { TY_FIBER, "__storage_get", 0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_NONE },
  { TY_FIBER, "__storage_set", 2, 2, BF_ANY, TY_POLY, BOPE_TEMPLATE,
    "({ sp_RbVal _t$t = $b1; SP_GC_ROOT_RBVAL(_t$t); sp_Fiber_attr_set($r, $e0, _t$t); _t$t; })", BOP_K(TY_SYMBOL) },
  { TY_FIBER, "__storage_set", 0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_NONE },
  { TY_FIBER, "alive?",        0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,    BOPE_TEMPLATE, "sp_Fiber_alive($r)" },
  /* Fiber#value: resume until the fiber finishes, answering the last value */
  { TY_FIBER, "value",         0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_TEMPLATE, "sp_Fiber_resume($r, sp_box_nil())" },
  { TY_FIBER, "kill",          0, 0,            BF_ANY, TY_FIBER,   BOPE_TEMPLATE, "sp_Fiber_kill($r)" },
  { TY_FIBER, "kill",          0, BOP_ARGC_ANY, BF_ANY, TY_FIBER,   BOPE_NONE },   /* the receiver */
  /* the running fiber's storage as a Hash copy (or nil), or replaced by one */
  { TY_FIBER, "storage",       0, 0,            BF_ANY, TY_POLY,    BOPE_TEMPLATE, "sp_Fiber_storage_hash($r)" },
  { TY_FIBER, "storage=",      1, 1, BF_ANY, TY_POLY, BOPE_TEMPLATE,
    "({ sp_RbVal _t$t = $b0; SP_GC_ROOT_RBVAL(_t$t); sp_Fiber_storage_assign($r, _t$t); _t$t; })" },
  { TY_FIBER, "blocking?",     0, 0,            BF_ANY, TY_BOOL,    BOPE_TEMPLATE, "sp_Fiber_blocking_p($r)" },
  { TY_FIBER, "inspect",       0, 0,            BF_ANY, TY_STRING,  BOPE_TEMPLATE, "sp_Fiber_inspect($r)" },
  { TY_FIBER, "to_s",          0, 0,            BF_ANY, TY_STRING,  BOPE_TEMPLATE, "sp_Fiber_inspect($r)" },

  /* Thread (a green thread on the scheduler). The universal queries come
     first for each handle kind (#3124). NULL encodes nil, as it does for an
     exception: a slot holding a handle may hold nil, so nil? is not a flat
     false (#3483); frozen? is emitted by emit_call_recv's arm, which reads
     the GC header's bit. */
  { TY_THREAD, "class",   0, 0, BF_ANY, TY_CLASS,  BOPE_NONE },
  { TY_THREAD, "frozen?", 0, 0, BF_ANY, TY_BOOL,   BOPE_NONE },
  { TY_THREAD, "nil?",    0, 0, BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "(($r) == NULL)" },
  { TY_THREAD, "itself",  0, 0, BF_ANY, TY_THREAD, BOPE_TEMPLATE, "$r" },
  { TY_THREAD, "inspect", 0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_Thread_inspect($r)" },
  { TY_THREAD, "to_s",    0, 0, BF_ANY, TY_STRING, BOPE_TEMPLATE, "sp_Thread_inspect($r)" },
  { TY_THREAD, "value",   0, 0,            BF_ANY, TY_POLY,   BOPE_TEMPLATE, "sp_Thread_value($r)" },
  { TY_THREAD, "value",   0, BOP_ARGC_ANY, BF_ANY, TY_POLY,   BOPE_NONE },
  /* join, kill, exit, terminate, raise, wakeup and run answer the receiver.
     join(limit) waits at most limit seconds, nil on a timeout; the limit
     goes through emit_float_expr, so a poly value is unboxed by
     sp_poly_to_f (CRuby's "can't convert X into Float") */
  { TY_THREAD, "join",      0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_join($r)" },
  { TY_THREAD, "join",      1, 1,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_join_timeout($r, $f0)" },
  { TY_THREAD, "join",      0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "kill",      0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_kill($r)" },
  { TY_THREAD, "kill",      0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "exit",      0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_kill($r)" },
  { TY_THREAD, "exit",      0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "terminate", 0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_kill($r)" },
  { TY_THREAD, "terminate", 0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "raise",     0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_THREAD_RAISE },
  { TY_THREAD, "wakeup",    0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_wakeup($r)" },
  { TY_THREAD, "wakeup",    0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "run",       0, 0,            BF_ANY, TY_THREAD, BOPE_TEMPLATE, "sp_Thread_run($r)" },
  { TY_THREAD, "run",       0, BOP_ARGC_ANY, BF_ANY, TY_THREAD, BOPE_NONE },
  { TY_THREAD, "alive?",    0, 0,            BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_Thread_alive($r)" },
  { TY_THREAD, "alive?",    0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,   BOPE_NONE },
  { TY_THREAD, "stop?",     0, 0,            BF_ANY, TY_BOOL,   BOPE_TEMPLATE, "sp_Thread_stop_p($r)" },
  { TY_THREAD, "stop?",     0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,   BOPE_NONE },
  { TY_THREAD, "report_on_exception",  0, 0,            BF_ANY, TY_BOOL, BOPE_TEMPLATE, "sp_Thread_get_report($r)" },
  { TY_THREAD, "report_on_exception",  0, BOP_ARGC_ANY, BF_ANY, TY_BOOL, BOPE_NONE },
  { TY_THREAD, "report_on_exception=", 1, 1,            BF_ANY, TY_BOOL, BOPE_THREAD_SET_REPORT },
  { TY_THREAD, "report_on_exception=", 0, BOP_ARGC_ANY, BF_ANY, TY_BOOL, BOPE_NONE },
  { TY_THREAD, "status",    0, 0,            BF_ANY, TY_POLY,   BOPE_TEMPLATE, "sp_Thread_status($r)" },
  { TY_THREAD, "status",    0, BOP_ARGC_ANY, BF_ANY, TY_POLY,   BOPE_NONE },
  { TY_THREAD, "name",      0, 0,            BF_ANY, TY_POLY,   BOPE_TEMPLATE, "sp_Thread_get_name($r)" },
  { TY_THREAD, "name",      0, BOP_ARGC_ANY, BF_ANY, TY_POLY,   BOPE_NONE },
  { TY_THREAD, "name=",     1, 1,            BF_ANY, TY_POLY,   BOPE_TEMPLATE, "sp_Thread_set_name($r, $b0)" },
  { TY_THREAD, "name=",     0, BOP_ARGC_ANY, BF_ANY, TY_POLY,   BOPE_NONE },
  { TY_THREAD, "equal?",    1, 1, BF_ANY, TY_BOOL, BOPE_TEMPLATE, "((void *)($r) == (void *)($e0))", BOP_K(TY_THREAD) },
  { TY_THREAD, "equal?",    0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,   BOPE_NONE },
  /* thread-local storage: t[:key] / t[:key] = v / t.key?(:key) by a symbol
     key directly, any other key through sp_thread_local_key.
     thread_variable_get / _set / ? are the thread-local spellings of the
     same store (`[]` is fiber-local in CRuby; this runtime keeps one table
     per thread for both) -- activesupport's IsolatedExecutionState reads it */
  { TY_THREAD, "[]",   1, 1, BF_ANY, TY_POLY, BOPE_TEMPLATE, "sp_Thread_tls_get($r, $e0)", BOP_K(TY_SYMBOL) },
  { TY_THREAD, "[]",   1, 1, BF_ANY, TY_POLY, BOPE_THREAD_TLS },
  { TY_THREAD, "[]",   0, BOP_ARGC_ANY, BF_ANY, TY_POLY, BOPE_NONE },
  { TY_THREAD, "[]=",  2, 2, BF_ANY, TY_POLY, BOPE_TEMPLATE, "sp_Thread_tls_set($r, $e0, $b1)", BOP_K(TY_SYMBOL) },
  { TY_THREAD, "[]=",  2, 2, BF_ANY, TY_POLY, BOPE_THREAD_TLS },
  { TY_THREAD, "[]=",  0, BOP_ARGC_ANY, BF_ANY, TY_POLY, BOPE_NONE },
  { TY_THREAD, "key?", 1, 1, BF_ANY, TY_BOOL, BOPE_TEMPLATE, "sp_Thread_tls_key($r, $e0)", BOP_K(TY_SYMBOL) },
  { TY_THREAD, "key?", 1, 1, BF_ANY, TY_BOOL, BOPE_THREAD_TLS },
  { TY_THREAD, "key?", 0, BOP_ARGC_ANY, BF_ANY, TY_BOOL, BOPE_NONE },
  { TY_THREAD, "thread_variable_get", 1, 1,            BF_ANY, TY_POLY, BOPE_THREAD_TLS },
  { TY_THREAD, "thread_variable_get", 0, BOP_ARGC_ANY, BF_ANY, TY_POLY, BOPE_NONE },
  { TY_THREAD, "thread_variable_set", 2, 2,            BF_ANY, TY_POLY, BOPE_THREAD_TLS },
  { TY_THREAD, "thread_variable_set", 0, BOP_ARGC_ANY, BF_ANY, TY_POLY, BOPE_NONE },
  { TY_THREAD, "thread_variable?",    1, 1,            BF_ANY, TY_BOOL, BOPE_THREAD_TLS },
  { TY_THREAD, "thread_variable?",    0, BOP_ARGC_ANY, BF_ANY, TY_BOOL, BOPE_NONE },
  { TY_THREAD, "keys", 0, 0, BF_ANY, TY_POLY_ARRAY, BOPE_TEMPLATE, "sp_Thread_tls_keys($r)" },

  /* Queue (a thread-safe FIFO on the scheduler). push and pop take a
     non_block flag and a timeout: keyword, so their arity counts the
     keyword hash; the emitter declines a form it does not take. push,
     close and clear answer the receiver. */
  { TY_QUEUE, "class",   0, 0, BF_ANY, TY_CLASS, BOPE_NONE },
  { TY_QUEUE, "frozen?", 0, 0, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_QUEUE, "nil?",    0, 0, BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "(($r) == NULL)" },
  { TY_QUEUE, "itself",  0, 0, BF_ANY, TY_QUEUE, BOPE_TEMPLATE, "$r" },
  { TY_QUEUE, "pop",     0, 2,            BF_ANY, TY_POLY,  BOPE_QUEUE_POP },
  { TY_QUEUE, "pop",     0, BOP_ARGC_ANY, BF_ANY, TY_POLY,  BOPE_NONE },
  { TY_QUEUE, "shift",   0, 2,            BF_ANY, TY_POLY,  BOPE_QUEUE_POP },
  { TY_QUEUE, "shift",   0, BOP_ARGC_ANY, BF_ANY, TY_POLY,  BOPE_NONE },
  { TY_QUEUE, "deq",     0, 2,            BF_ANY, TY_POLY,  BOPE_QUEUE_POP },
  { TY_QUEUE, "deq",     0, BOP_ARGC_ANY, BF_ANY, TY_POLY,  BOPE_NONE },
  { TY_QUEUE, "push",    1, 3,            BF_ANY, TY_QUEUE, BOPE_QUEUE_PUSH },
  { TY_QUEUE, "push",    0, BOP_ARGC_ANY, BF_ANY, TY_QUEUE, BOPE_NONE },
  { TY_QUEUE, "<<",      1, 3,            BF_ANY, TY_QUEUE, BOPE_QUEUE_PUSH },
  { TY_QUEUE, "<<",      0, BOP_ARGC_ANY, BF_ANY, TY_QUEUE, BOPE_NONE },
  { TY_QUEUE, "enq",     1, 3,            BF_ANY, TY_QUEUE, BOPE_QUEUE_PUSH },
  { TY_QUEUE, "enq",     0, BOP_ARGC_ANY, BF_ANY, TY_QUEUE, BOPE_NONE },
  { TY_QUEUE, "close",   0, 0,            BF_ANY, TY_QUEUE, BOPE_TEMPLATE, "({ sp_queue *_t$t = $r; sp_Queue_close(_t$t); _t$t; })" },
  { TY_QUEUE, "close",   0, BOP_ARGC_ANY, BF_ANY, TY_QUEUE, BOPE_NONE },
  { TY_QUEUE, "clear",   0, 0,            BF_ANY, TY_QUEUE, BOPE_TEMPLATE, "({ sp_queue *_t$t = $r; sp_Queue_clear(_t$t); _t$t; })" },
  { TY_QUEUE, "clear",   0, BOP_ARGC_ANY, BF_ANY, TY_QUEUE, BOPE_NONE },
  { TY_QUEUE, "size",    0, 0,            BF_ANY, TY_INT,   BOPE_TEMPLATE, "sp_Queue_size($r)" },
  { TY_QUEUE, "size",    0, BOP_ARGC_ANY, BF_ANY, TY_INT,   BOPE_NONE },
  { TY_QUEUE, "length",  0, 0,            BF_ANY, TY_INT,   BOPE_TEMPLATE, "sp_Queue_size($r)" },
  { TY_QUEUE, "length",  0, BOP_ARGC_ANY, BF_ANY, TY_INT,   BOPE_NONE },
  { TY_QUEUE, "max",     0, 0,            BF_ANY, TY_INT,   BOPE_TEMPLATE, "sp_Queue_max($r)" },
  { TY_QUEUE, "max",     0, BOP_ARGC_ANY, BF_ANY, TY_INT,   BOPE_NONE },
  { TY_QUEUE, "num_waiting", 0, 0,            BF_ANY, TY_INT, BOPE_TEMPLATE, "sp_Queue_num_waiting($r)" },
  { TY_QUEUE, "num_waiting", 0, BOP_ARGC_ANY, BF_ANY, TY_INT, BOPE_NONE },
  { TY_QUEUE, "empty?",  0, 0,            BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "sp_Queue_empty($r)" },
  { TY_QUEUE, "empty?",  0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_QUEUE, "closed?", 0, 0,            BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "sp_Queue_closed($r)" },
  { TY_QUEUE, "closed?", 0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,  BOPE_NONE },

  /* Mutex. lock and unlock answer the receiver; synchronize, the block's
     value, is emitted by the generic block handler; sleep answers nil on a
     timeout, else the seconds slept. */
  { TY_MUTEX, "class",       0, 0, BF_ANY, TY_CLASS, BOPE_NONE },
  { TY_MUTEX, "frozen?",     0, 0, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_MUTEX, "nil?",        0, 0, BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "(($r) == NULL)" },
  { TY_MUTEX, "itself",      0, 0, BF_ANY, TY_MUTEX, BOPE_TEMPLATE, "$r" },
  { TY_MUTEX, "lock",        0, 0,            BF_ANY, TY_MUTEX, BOPE_TEMPLATE, "({ sp_mutex *_t$t = $r; sp_Mutex_lock(_t$t); _t$t; })" },
  { TY_MUTEX, "lock",        0, BOP_ARGC_ANY, BF_ANY, TY_MUTEX, BOPE_NONE },
  { TY_MUTEX, "unlock",      0, 0,            BF_ANY, TY_MUTEX, BOPE_TEMPLATE, "({ sp_mutex *_t$t = $r; sp_Mutex_unlock(_t$t); _t$t; })" },
  { TY_MUTEX, "unlock",      0, BOP_ARGC_ANY, BF_ANY, TY_MUTEX, BOPE_NONE },
  { TY_MUTEX, "try_lock",    0, 0,            BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "sp_Mutex_try_lock($r)" },
  { TY_MUTEX, "try_lock",    0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_MUTEX, "locked?",     0, 0,            BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "sp_Mutex_locked($r)" },
  { TY_MUTEX, "locked?",     0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_MUTEX, "owned?",      0, 0,            BF_ANY, TY_BOOL,  BOPE_TEMPLATE, "sp_Mutex_owned($r)" },
  { TY_MUTEX, "owned?",      0, BOP_ARGC_ANY, BF_ANY, TY_BOOL,  BOPE_NONE },
  { TY_MUTEX, "synchronize", 0, BOP_ARGC_ANY, BF_ANY, TY_POLY,  BOPE_NONE },
  { TY_MUTEX, "sleep",       0, 1,            BF_ANY, TY_POLY,  BOPE_MUTEX_SLEEP },
  { TY_MUTEX, "sleep",       0, BOP_ARGC_ANY, BF_ANY, TY_POLY,  BOPE_NONE },

  /* ConditionVariable. #wait answers nil (timed out) or the Integer
     seconds slept, as CRuby; signal and broadcast answer the receiver. */
  { TY_CONDVAR, "class",     0, 0, BF_ANY, TY_CLASS,   BOPE_NONE },
  { TY_CONDVAR, "frozen?",   0, 0, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_CONDVAR, "nil?",      0, 0, BF_ANY, TY_BOOL,    BOPE_TEMPLATE, "(($r) == NULL)" },
  { TY_CONDVAR, "itself",    0, 0, BF_ANY, TY_CONDVAR, BOPE_TEMPLATE, "$r" },
  { TY_CONDVAR, "wait",      1, 2,            BF_ANY, TY_POLY,    BOPE_CONDVAR_WAIT },
  { TY_CONDVAR, "wait",      0, BOP_ARGC_ANY, BF_ANY, TY_POLY,    BOPE_NONE },
  { TY_CONDVAR, "signal",    0, 0,            BF_ANY, TY_CONDVAR, BOPE_TEMPLATE, "({ sp_condvar *_t$t = $r; sp_CondVar_signal(_t$t); _t$t; })" },
  { TY_CONDVAR, "signal",    0, BOP_ARGC_ANY, BF_ANY, TY_CONDVAR, BOPE_NONE },
  { TY_CONDVAR, "broadcast", 0, 0,            BF_ANY, TY_CONDVAR, BOPE_TEMPLATE, "({ sp_condvar *_t$t = $r; sp_CondVar_broadcast(_t$t); _t$t; })" },
  { TY_CONDVAR, "broadcast", 0, BOP_ARGC_ANY, BF_ANY, TY_CONDVAR, BOPE_NONE },

  /* IO / File (sp_File *, NULL is nil): the calls typed by name and arity.
     sync= (its argument), the socket and io/console methods (features),
     the *_nonblock family (exception: false) and the each_* iterators
     (they type the block parameter) stay in infer_call_inner; a name with
     no rule there is poly. */
  { TY_IO, "respond_to?",    0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },  /* true or false whatever the name */
  { TY_IO, "read",           0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_read($r)", TY_UNKNOWN },
  { TY_IO, "read",           0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "gets",           0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_gets($r)", TY_UNKNOWN },
  { TY_IO, "gets",           0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "readline",       0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "path",           0, 127, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_path($r)", TY_UNKNOWN },
  { TY_IO, "to_path",        0, 127, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_path($r)", TY_UNKNOWN },
  { TY_IO, "getc",           0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_getc($r)", TY_UNKNOWN },
  { TY_IO, "getc",           0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "readchar",       0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_readchar($r)", TY_UNKNOWN },
  { TY_IO, "readchar",       0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "readpartial",    0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "sysread",        0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "ftype",          0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_stat_ftype($r)", TY_UNKNOWN },
  { TY_IO, "ftype",          0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "pread",          0, 127, BF_ANY, TY_STRING,   BOPE_NONE },
  { TY_IO, "readlines",      0, 127, BF_ANY, TY_STR_ARRAY, BOPE_NONE },
  { TY_IO, "write",          0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "syswrite",       0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "pos",            0, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_tell($r)", TY_UNKNOWN },
  { TY_IO, "tell",           0, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_tell($r)", TY_UNKNOWN },
  { TY_IO, "seek",           1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_seek($r, $i0, 0)", TY_UNKNOWN },
  { TY_IO, "seek",           2, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_seek($r, $i0, $i1)", TY_UNKNOWN },
  { TY_IO, "seek",           0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "rewind",         0, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_rewind($r)", TY_UNKNOWN },
  { TY_IO, "fileno",         0, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fileno($r)", TY_UNKNOWN },
  { TY_IO, "to_i",           0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fileno($r)", TY_UNKNOWN },
  { TY_IO, "to_i",           0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "lineno",         0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_lineno($r)", TY_UNKNOWN },
  { TY_IO, "lineno",         0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "lineno=",        1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_set_lineno($r, $i0)", TY_UNKNOWN },
  { TY_IO, "lineno=",        0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "pos=",           1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "({ sp_int _t$t = $i0; sp_File_seek($r, _t$t, 0); _t$t; })", TY_UNKNOWN },
  { TY_IO, "pos=",           0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "truncate",       1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_truncate($r, $i0)", TY_UNKNOWN },
  { TY_IO, "truncate",       0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "fsync",          0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fsync($r)", TY_UNKNOWN },
  { TY_IO, "fsync",          0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "fdatasync",      0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fsync($r)", TY_UNKNOWN },
  { TY_IO, "fdatasync",      0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "getbyte",        0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_getbyte($r)", TY_UNKNOWN },
  { TY_IO, "getbyte",        0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "sysseek",        1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_sysseek($r, $i0, 0)", TY_UNKNOWN },
  { TY_IO, "sysseek",        2, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_sysseek($r, $i0, $i1)", TY_UNKNOWN },
  { TY_IO, "sysseek",        0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "size",           0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_size($r)", TY_UNKNOWN },
  { TY_IO, "size",           0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "chmod",          1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "({ sp_file_chmod($i0, sp_File_path($r)); (sp_int)0; })", TY_UNKNOWN },
  { TY_IO, "chmod",          0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "mode",           0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_mode($r)", TY_UNKNOWN },
  { TY_IO, "mode",           0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "readbyte",       0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_readbyte($r)", TY_UNKNOWN },
  { TY_IO, "readbyte",       0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "fcntl",          1,   1, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fcntl($r, $i0, 0)", TY_UNKNOWN },
  { TY_IO, "fcntl",          2, 127, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_File_fcntl($r, $i0, $i1)", TY_UNKNOWN },
  { TY_IO, "fcntl",          0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "pwrite",         0, 127, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "chown",          2,   2, BF_ANY, TY_INT,      BOPE_NONE },  /* #3104 */
  { TY_IO, "close",          0, 127, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_close($r); sp_box_nil(); })", TY_UNKNOWN },  /* nil (#2801) */
  { TY_IO, "flock",          1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "sp_File_flock($r, $i0)", TY_UNKNOWN },
  { TY_IO, "flock",          0, 127, BF_ANY, TY_POLY,     BOPE_NONE },  /* 0, or false for a held LOCK_NB */
  { TY_IO, "print",          0, 127, BF_ANY, TY_NIL,      BOPE_NONE },
  { TY_IO, "puts",           0, 127, BF_ANY, TY_NIL,      BOPE_NONE },
  { TY_IO, "flush",          0, 127, BF_ANY, TY_IO,       BOPE_TEMPLATE, "({ sp_File *_t$t = $r; sp_File_flush(_t$t); _t$t; })", TY_UNKNOWN },  /* self (#2799) */
  { TY_IO, "binmode",        0, 127, BF_ANY, TY_IO,       BOPE_TEMPLATE, "({ sp_File *_t$t = $r; sp_File_set_binmode(_t$t); _t$t; })", TY_UNKNOWN },
  { TY_IO, "closed?",        0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_closed_p($r)", TY_UNKNOWN },
  { TY_IO, "eof?",           0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_eof_p($r)", TY_UNKNOWN },
  { TY_IO, "eof",            0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_eof_p($r)", TY_UNKNOWN },
  { TY_IO, "tty?",           0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_tty_p($r)", TY_UNKNOWN },
  { TY_IO, "isatty",         0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_tty_p($r)", TY_UNKNOWN },
  { TY_IO, "sync",           0, 127, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_sync_p($r)", TY_UNKNOWN },
  { TY_IO, "autoclose?",     0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_autoclose_p($r)", TY_UNKNOWN },
  { TY_IO, "autoclose?",     0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "==",             0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "equal?",         0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "eql?",           0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "binmode?",       0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_binmode_p($r)", TY_UNKNOWN },
  { TY_IO, "binmode?",       0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "close_on_exec?", 0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_File_close_on_exec_p($r)", TY_UNKNOWN },
  { TY_IO, "close_on_exec?", 0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "close_on_exec=", 0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "autoclose=",     0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "file?",          0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 0)", TY_UNKNOWN },
  { TY_IO, "file?",          0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },  /* File::Stat predicates: a stat is the handle itself */
  { TY_IO, "directory?",     0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 1)", TY_UNKNOWN },
  { TY_IO, "directory?",     0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "symlink?",       0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 2)", TY_UNKNOWN },
  { TY_IO, "symlink?",       0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "owned?",         0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 3)", TY_UNKNOWN },
  { TY_IO, "owned?",         0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "grpowned?",      0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 4)", TY_UNKNOWN },
  { TY_IO, "grpowned?",      0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "setuid?",        0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 5)", TY_UNKNOWN },
  { TY_IO, "setuid?",        0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "setgid?",        0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 6)", TY_UNKNOWN },
  { TY_IO, "setgid?",        0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "sticky?",        0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 7)", TY_UNKNOWN },
  { TY_IO, "sticky?",        0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "socket?",        0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_type_pred($r, 8)", TY_UNKNOWN },
  { TY_IO, "socket?",        0, 127, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "uid",            0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 0)", TY_UNKNOWN },  /* File::Stat fields (#3765); size? is int-or-nil (sentinel) */
  { TY_IO, "gid",            0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 1)", TY_UNKNOWN },
  { TY_IO, "nlink",          0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 2)", TY_UNKNOWN },
  { TY_IO, "dev",            0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 3)", TY_UNKNOWN },
  { TY_IO, "ino",            0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 4)", TY_UNKNOWN },
  { TY_IO, "blksize",        0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 5)", TY_UNKNOWN },
  { TY_IO, "blocks",         0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 6)", TY_UNKNOWN },
  { TY_IO, "rdev",           0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_field($r, 7)", TY_UNKNOWN },
  { TY_IO, "size?",          0,   0, BF_ANY, TY_INT,      BOPE_TEMPLATE, "sp_stat_pred($r, 7)", TY_UNKNOWN },
  { TY_IO, "pipe?",          0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 0)", TY_UNKNOWN },
  { TY_IO, "zero?",          0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 1)", TY_UNKNOWN },
  { TY_IO, "readable?",      0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 2)", TY_UNKNOWN },
  { TY_IO, "writable?",      0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 3)", TY_UNKNOWN },
  { TY_IO, "executable?",    0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 4)", TY_UNKNOWN },
  { TY_IO, "blockdev?",      0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 5)", TY_UNKNOWN },
  { TY_IO, "chardev?",       0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "sp_stat_pred($r, 6)", TY_UNKNOWN },
  { TY_IO, "inspect",        0,   0, BF_ANY, TY_STRING,   BOPE_TEMPLATE, "sp_File_inspect($r)", TY_UNKNOWN },
  { TY_IO, "nil?",           0,   0, BF_ANY, TY_BOOL,     BOPE_TEMPLATE, "(($r) == NULL)", TY_UNKNOWN },
  { TY_IO, "is_a?",          1,   1, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "kind_of?",       1,   1, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "instance_of?",   1,   1, BF_ANY, TY_BOOL,     BOPE_NONE },
  { TY_IO, "hash",           0,   0, BF_ANY, TY_INT,      BOPE_NONE },  /* Object's, on the handle */
  { TY_IO, "object_id",      0,   0, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "__id__",         0,   0, BF_ANY, TY_INT,      BOPE_NONE },
  { TY_IO, "wait_readable",  0, 127, BF_ANY, TY_IO,       BOPE_NONE },  /* the handle or nil */
  { TY_IO, "wait_writable",  0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "wait_priority",  0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "wait",           0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "to_io",          0,   0, BF_ANY, TY_IO,       BOPE_TEMPLATE, "($r)", TY_UNKNOWN },
  { TY_IO, "to_io",          0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "reopen",         1, 127, BF_ANY, TY_IO,       BOPE_TEMPLATE, "sp_File_reopen_io($r, $e0)", BOP_K(TY_IO) },
  { TY_IO, "reopen",         0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "stat",           0,   0, BF_ANY, TY_IO,       BOPE_TEMPLATE, "sp_io_stat_handle($r)", TY_UNKNOWN },
  { TY_IO, "stat",           0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "lstat",          0,   0, BF_ANY, TY_IO,       BOPE_TEMPLATE, "sp_file_lstat_handle(sp_File_path($r))", TY_UNKNOWN },
  { TY_IO, "lstat",          0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "<<",             0, 127, BF_ANY, TY_IO,       BOPE_NONE },
  { TY_IO, "ungetbyte",      1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_ungetbyte($r, $i0); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "ungetbyte",      0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "advise",         1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, sp_sym_to_s($e0), 0, 0); sp_box_nil(); })", BOP_K(TY_SYMBOL) },
  { TY_IO, "advise",         1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, $s0, 0, 0); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "advise",         2,   2, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, sp_sym_to_s($e0), $i1, 0); sp_box_nil(); })", BOP_K(TY_SYMBOL) },
  { TY_IO, "advise",         2,   2, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, $s0, $i1, 0); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "advise",         3, 127, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, sp_sym_to_s($e0), $i1, $i2); sp_box_nil(); })", BOP_K(TY_SYMBOL) },
  { TY_IO, "advise",         3, 127, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_advise($r, $s0, $i1, $i2); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "advise",         0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "close_read",     0,   0, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_close_half($r, 1); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "close_read",     0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "close_write",    0,   0, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File_close_half($r, 0); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "close_write",    0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "putc",           1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "sp_File_putc($r, $b0)", TY_UNKNOWN },
  { TY_IO, "putc",           0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "printf",         0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "ungetc",         1,   1, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "sp_File_ungetc($r, $b0)", TY_UNKNOWN },
  { TY_IO, "ungetc",         0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "pid",            0,   0, BF_ANY, TY_POLY,     BOPE_TEMPLATE, "({ sp_File *_t$t = $r; SP_IO_OPEN(_t$t); sp_box_nil(); })", TY_UNKNOWN },
  { TY_IO, "pid",            0, 127, BF_ANY, TY_POLY,     BOPE_NONE },
  { TY_IO, "mtime",          0,   0, BF_ANY, TY_TIME,     BOPE_TEMPLATE, "sp_stat_handle_time($r, 0)", TY_UNKNOWN },
  { TY_IO, "mtime",          0, 127, BF_ANY, TY_TIME,     BOPE_NONE },
  { TY_IO, "atime",          0,   0, BF_ANY, TY_TIME,     BOPE_TEMPLATE, "sp_stat_handle_time($r, 1)", TY_UNKNOWN },
  { TY_IO, "atime",          0, 127, BF_ANY, TY_TIME,     BOPE_NONE },
  { TY_IO, "ctime",          0,   0, BF_ANY, TY_TIME,     BOPE_TEMPLATE, "sp_stat_handle_time($r, 2)", TY_UNKNOWN },
  { TY_IO, "ctime",          0, 127, BF_ANY, TY_TIME,     BOPE_NONE },
  { TY_IO, "birthtime",      0,   0, BF_ANY, TY_TIME,     BOPE_TEMPLATE, "sp_file_birthtime(sp_File_path($r))", TY_UNKNOWN },
  { TY_IO, "birthtime",      0, 127, BF_ANY, TY_TIME,     BOPE_NONE },

  /* Regexp: result kinds; emission stays in emit_call_body. */
  { TY_REGEX, "match?",          0, 127, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "===",             0, 127, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "match",           0, 127, BF_REQUIRED, TY_POLY,          BOPE_NONE },  /* the block's value, nil on a miss (#3642) */
  { TY_REGEX, "match",           0, 127, BF_NONE,     TY_MATCHDATA,     BOPE_NONE },
  { TY_REGEX, "=~",              0, 127, BF_ANY,      TY_POLY,          BOPE_NONE },
  { TY_REGEX, "~",               0,   0, BF_ANY,      TY_POLY,          BOPE_NONE },  /* ~ /re/ is /re/ =~ $_ */
  { TY_REGEX, "source",          0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_re_source_str((void *)($r))", 0 },
  { TY_REGEX, "source",          0, 127, BF_ANY,      TY_STRING,        BOPE_NONE },
  { TY_REGEX, "inspect",         0, 127, BF_ANY,      TY_STRING,        BOPE_NONE },
  { TY_REGEX, "to_s",            0, 127, BF_ANY,      TY_STRING,        BOPE_NONE },
  { TY_REGEX, "names",           0,   0, BF_ANY,      TY_STR_ARRAY,  BOPE_TEMPLATE, "sp_Regexp_names((void *)($r))", 0 },
  { TY_REGEX, "names",           0, 127, BF_ANY,      TY_STR_ARRAY,     BOPE_NONE },
  { TY_REGEX, "named_captures",  0,   0, BF_ANY,      TY_STR_POLY_HASH, BOPE_TEMPLATE, "({ const void *_t$t = (const void *)($r); sp_StrPolyHash *_t$u = sp_StrPolyHash_new(); SP_GC_ROOT(_t$u); int _n$v = re_num_named((const mrb_regexp_pattern *)_t$t); for (int _t$v = 0; _t$v < _n$v; _t$v++) { int _g$v = 0; const char *_nm$v = re_named_name((const mrb_regexp_pattern *)_t$t, _t$v, &_g$v); if (_nm$v) { sp_RbVal _cur$v = sp_StrPolyHash_get(_t$u, _nm$v); sp_IntArray *_ia$v; if (_cur$v.tag == SP_TAG_NIL) { _ia$v = sp_IntArray_new(); sp_StrPolyHash_set(_t$u, sp_str_dup(_nm$v), sp_box_int_array(_ia$v)); }\nelse _ia$v = (sp_IntArray *)_cur$v.v.p; sp_IntArray_push(_ia$v, _g$v); } } _t$u; })", 0 },
  { TY_REGEX, "named_captures",  0, 127, BF_ANY,      TY_STR_POLY_HASH, BOPE_NONE },  /* {name => [group indices]} */
  { TY_REGEX, "freeze",          0,   0, BF_ANY,      TY_REGEX,      BOPE_TEMPLATE, "$r", 0 },
  { TY_REGEX, "freeze",          0, 127, BF_ANY,      TY_REGEX,         BOPE_NONE },
  { TY_REGEX, "dup",             0,   0, BF_ANY,      TY_REGEX,      BOPE_TEMPLATE, "$r", 0 },
  { TY_REGEX, "dup",             0, 127, BF_ANY,      TY_REGEX,         BOPE_NONE },
  { TY_REGEX, "clone",           0,   0, BF_ANY,      TY_REGEX,      BOPE_TEMPLATE, "$r", 0 },
  { TY_REGEX, "clone",           0, 127, BF_ANY,      TY_REGEX,         BOPE_NONE },
  { TY_REGEX, "itself",          0,   0, BF_ANY,      TY_REGEX,      BOPE_TEMPLATE, "$r", 0 },
  { TY_REGEX, "itself",          0, 127, BF_ANY,      TY_REGEX,         BOPE_NONE },
  { TY_REGEX, "frozen?",         0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), 1)", 0 },
  { TY_REGEX, "frozen?",         0, 127, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "fixed_encoding?", 0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), FALSE)", 0 },
  { TY_REGEX, "fixed_encoding?", 0, 127, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "casefold?",       0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_re_casefold_p((void *)($r))", 0 },
  { TY_REGEX, "casefold?",       0, 127, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "==",              1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(sp_re_eq((void *)($r), (void *)($e0)))", BOP_K(TY_REGEX) },
  { TY_REGEX, "==",              1,   1, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "!=",              1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(!sp_re_eq((void *)($r), (void *)($e0)))", BOP_K(TY_REGEX) },
  { TY_REGEX, "!=",              1,   1, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "equal?",          1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void *)($r) == (void *)($e0))", BOP_K(TY_REGEX) },
  { TY_REGEX, "equal?",          1,   1, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "eql?",            1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_re_eq((void *)($r), (void *)($e0))", BOP_K(TY_REGEX) },
  { TY_REGEX, "eql?",            1,   1, BF_ANY,      TY_BOOL,          BOPE_NONE },
  { TY_REGEX, "encoding",        0,   0, BF_ANY,      TY_POLY,       BOPE_TEMPLATE, "((void)($r), sp_box_encoding(sp_encoding_us_ascii()))", 0 },
  { TY_REGEX, "encoding",        0, 127, BF_ANY,      TY_POLY,          BOPE_NONE },  /* a boxed Encoding */
  { TY_REGEX, "options",         0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_re_options((void *)($r))", 0 },
  { TY_REGEX, "options",         0, 127, BF_ANY,      TY_INT,           BOPE_NONE },
  { TY_REGEX, "timeout",         0,   0, BF_ANY,      TY_POLY,       BOPE_TEMPLATE, "((void)($r), sp_box_nil())", 0 },
  { TY_REGEX, "timeout",         0, 127, BF_ANY,      TY_POLY,          BOPE_NONE },  /* nil: no per-instance timeout */
  { TY_REGEX, "hash",            0,   0, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "(sp_int)sp_re_hash((void *)($r))", 0 },

  /* Symbol: result kinds; emission stays in emit_call_body. <=> and
     casecmp/casecmp? are typed by the operand (nil for a non-Symbol). */
  { TY_SYMBOL, "to_s",            0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_sym_to_s_chilled($r)", 0 },
  { TY_SYMBOL, "id2name",         0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_sym_to_s_chilled($r)", 0 },
  { TY_SYMBOL, "name",            0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_uminus_val(sp_sym_to_s($r))", 0 },
  { TY_SYMBOL, "inspect",         0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_sym_inspect($r)", 0 },
  { TY_SYMBOL, "upcase",          0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_upcase(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "downcase",        0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_downcase(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "capitalize",      0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_capitalize(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "swapcase",        0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_swapcase(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "to_sym",          0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "$r", 0 },
  { TY_SYMBOL, "intern",          0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "$r", 0 },
  { TY_SYMBOL, "itself",          0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "$r", 0 },
  { TY_SYMBOL, "succ",            0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_succ(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "next",            0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern(sp_str_succ(sp_sym_to_s($r)))", 0 },
  { TY_SYMBOL, "length",          0, 127, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_length(sp_sym_to_s($r))", 0 },
  { TY_SYMBOL, "size",            0, 127, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_length(sp_sym_to_s($r))", 0 },
  { TY_SYMBOL, "empty?",          0, 127, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(strlen(sp_sym_to_s($r)) == 0)", 0 },
  { TY_SYMBOL, "==",              1, 127, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "($r == $e0)", 0 },
  { TY_SYMBOL, "==",          0, 127, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_SYMBOL, "!=",              1, 127, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(!($r == $e0))", 0 },
  { TY_SYMBOL, "!=",          0, 127, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_SYMBOL, "[]",              2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_sub_range(sp_sym_to_s($r), $i0, $i1)", 0 },
  { TY_SYMBOL, "[]",          1,   2, BF_ANY, TY_STRING,  BOPE_NONE },
  { TY_SYMBOL, "slice",           2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_sub_range(sp_sym_to_s($r), $i0, $i1)", 0 },
  { TY_SYMBOL, "slice",       1,   2, BF_ANY, TY_STRING,  BOPE_NONE },
  { TY_SYMBOL, "start_with?",     1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_start_with(sp_sym_to_s($r), $s0)", BOP_K(TY_STRING) | BOP_K(TY_POLY) },
  { TY_SYMBOL, "start_with?", 1,   1, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_SYMBOL, "end_with?",       1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_end_with(sp_sym_to_s($r), $s0)", BOP_K(TY_STRING) | BOP_K(TY_POLY) },
  { TY_SYMBOL, "end_with?",   1,   1, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_SYMBOL, "match?",      1,   1, BF_ANY, TY_BOOL,    BOPE_NONE },
  { TY_SYMBOL, "casecmp",         1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "sp_str_casecmp(sp_sym_to_s($r), sp_sym_to_s($e0))", BOP_K(TY_SYMBOL) },
  { TY_SYMBOL, "casecmp",         1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), (void)($e0), 0)", 0 },
  { TY_SYMBOL, "casecmp?",        1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "(sp_str_casecmp(sp_sym_to_s($r), sp_sym_to_s($e0)) == 0)", BOP_K(TY_SYMBOL) },
  { TY_SYMBOL, "casecmp?",        1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), (void)($e0), 0)", 0 },

  /* Method and Proc: the result kinds read off the name and arity.
     call/()/[] (the target's or the proc's return), bind_call, receiver,
     composition (<< / >>) and Proc identity against a Proc are typed in
     infer_call_inner. */
  { TY_METHOD, "to_proc",         0,   0, BF_ANY, TY_PROC,       BOPE_NONE },
  { TY_METHOD, "original_name",   0,   0, BF_ANY, TY_SYMBOL,     BOPE_NONE },  /* reflection (#3247) */
  { TY_METHOD, "name",            0,   0, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "sp_sym_intern((const char *)($r)->name)", 0 },
  { TY_METHOD, "parameters",      0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_METHOD, "source_location", 0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },
  { TY_METHOD, "dup",             0,   0, BF_ANY,      TY_METHOD,     BOPE_TEMPLATE, "$r", 0 },
  { TY_METHOD, "clone",           0,   0, BF_ANY,      TY_METHOD,     BOPE_TEMPLATE, "$r", 0 },
  { TY_METHOD, "unbind",          0,   0, BF_ANY, TY_METHOD,     BOPE_NONE },
  { TY_METHOD, "super_method",    0,   0, BF_ANY, TY_METHOD,     BOPE_NONE },
  { TY_METHOD, "inspect",         0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_method_desc_cstr($r)", 0 },
  { TY_METHOD, "to_s",            0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_method_desc_cstr($r)", 0 },
  { TY_METHOD, "box",             0,   0, BF_ANY,      TY_NIL,        BOPE_TEMPLATE, "((void)($r), sp_box_nil())", 0 },  /* namespace-less: never boxed */
  { TY_METHOD, "owner",           0,   0, BF_ANY, TY_CLASS,      BOPE_NONE },  /* #2701 */
  { TY_METHOD, "arity",           0,   0, BF_ANY, TY_INT,        BOPE_NONE },
  { TY_METHOD, "==",              1,   1, BF_ANY, TY_BOOL,       BOPE_NONE },
  { TY_METHOD, "eql?",            1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "({ sp_BoundMethod *_t$t = $r; sp_BoundMethod *_t$u = $e0; (sp_bool)(_t$t->self == _t$u->self && _t$t->fn == _t$u->fn); })", BOP_K(TY_METHOD) },
  { TY_METHOD, "eql?",            1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (void)($b0), (sp_bool)0)", 0 },
  { TY_METHOD, "equal?",          1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "({ sp_BoundMethod *_t$t = $r; sp_BoundMethod *_t$u = $e0; (sp_bool)(_t$t->self == _t$u->self && _t$t->fn == _t$u->fn); })", BOP_K(TY_METHOD) },
  { TY_METHOD, "equal?",          1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (void)($b0), (sp_bool)0)", 0 },
  { TY_METHOD, "bind",            1,   1, BF_ANY, TY_METHOD,     BOPE_NONE },  /* #2676 */
  { TY_PROC,   "to_proc",         0,   0, BF_ANY, TY_PROC,       BOPE_NONE },  /* self (#3687) */
  { TY_PROC,   "===",             1,   1, BF_ANY, TY_POLY,       BOPE_NONE },  /* the proc's value, boxed (#3818) */
  { TY_PROC,   "parameters",      0,   1, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },  /* parameters(lambda:) is the same shape */
  { TY_PROC, "arity",           0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_proc_arity($r)", 0 },
  { TY_PROC, "lambda?",         0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_proc_lambda_p($r)", 0 },
  { TY_PROC, "frozen?",         0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(($r)->frozen)", 0 },
  { TY_PROC,   "source_location", 0,   0, BF_ANY, TY_POLY_ARRAY, BOPE_NONE },  /* [file, line] */
  { TY_PROC, "inspect",         0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_proc_inspect($r)", 0 },
  { TY_PROC, "to_s",            0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_proc_inspect($r)", 0 },
  { TY_PROC, "freeze",          0,   0, BF_ANY,      TY_PROC,       BOPE_TEMPLATE, "({ sp_Proc *_t$t = $r; _t$t->frozen = TRUE; _t$t; })", 0 },
  { TY_PROC, "dup",             0,   0, BF_ANY,      TY_PROC,       BOPE_TEMPLATE, "sp_proc_dup($r, 0)", 0 },
  { TY_PROC, "clone",           0,   0, BF_ANY,      TY_PROC,       BOPE_TEMPLATE, "sp_proc_dup($r, 1)", 0 },
  { TY_PROC, "itself",          0,   0, BF_ANY,      TY_PROC,       BOPE_TEMPLATE, "$r", 0 },
  { TY_PROC, "equal?",          1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(($r) == ($e0))", BOP_K(TY_PROC) },
  { TY_PROC, "eql?",            1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(sp_proc_root($r) == sp_proc_root($e0))", BOP_K(TY_PROC) },
  { TY_PROC, "==",              1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(sp_proc_root($r) == sp_proc_root($e0))", BOP_K(TY_PROC) },

  /* Integer and Float: the result kinds read off the name, arity and block
     form. pow, clamp, coerce, Float#fdiv(Complex), Float <=> Rational,
     Float#floor/ceil/round/truncate and the promote-mode widening read the
     operands and stay in infer_call_inner. */
  /* Integer: the arms that read only the receiver (rendered once, possibly
     behind the nullable-Integer guard) and the arguments */
  { TY_INT,   "to_s",        0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? sp_str_frozen_empty : sp_int_to_s(_t$t); })" },  /* a nullable Integer: nil's own answers */
  { TY_INT,   "inspect",     0, 127, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(_t$t); })" },
  { TY_INT,   "to_f",        0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? 0.0 : ((sp_float)_t$t); })" },  /* #4070 */
  { TY_INT,   "to_i",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? 0 : _t$t; })" },
  { TY_INT,   "abs",         0, 127, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_int_abs($r)" },
  { TY_INT,   "size",        0,   0, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "((sp_int)sizeof(sp_int))" },
  { TY_INT,   "to_int",      1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "ord",         1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "infinite?",   1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "((void)($r), SP_INT_NIL)" },
  { TY_INT,   "abs2",        1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "sp_int_mul($r, $r)" },
  { TY_INT,   "real",        1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "imaginary",   1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_INT,   "imag",        1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_INT,   "conj",        1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "conjugate",   1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "arg",         1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_INT,   "angle",       1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_INT,   "phase",       1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_INT,   "nonzero?",    1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) == 0 ? SP_INT_NIL : ($r))" },
  { TY_INT,   "ceil",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "ceil",        0, 127, BF_ANY,      TY_INT,         BOPE_NONE },  /* no precision: self */
  { TY_INT,   "floor",       0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "floor",       0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "round",       0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "round",       0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "truncate",    0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "truncate",    0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "divmod",      1,   1, BF_ANY,      TY_INT_ARRAY,   BOPE_NONE },
  { TY_INT,   "allbits?",    1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_INT,   "anybits?",    1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_INT,   "nobits?",     1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { TY_INT,   "even?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? (sp_raise_cls(\"NoMethodError\", \"undefined method 'even?' for nil\"), FALSE) : (_t$t % 2 == 0); })" },
  { TY_INT,   "odd?",        0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? (sp_raise_cls(\"NoMethodError\", \"undefined method 'odd?' for nil\"), FALSE) : (_t$t % 2 != 0); })" },
  { TY_INT,   "zero?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? (sp_raise_cls(\"NoMethodError\", \"undefined method 'zero?' for nil\"), FALSE) : (_t$t == 0); })" },
  { TY_INT,   "positive?",   0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? (sp_raise_cls(\"NoMethodError\", \"undefined method 'positive?' for nil\"), FALSE) : (_t$t > 0); })" },
  { TY_INT,   "negative?",   0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? (sp_raise_cls(\"NoMethodError\", \"undefined method 'negative?' for nil\"), FALSE) : (_t$t < 0); })" },
  { TY_INT,   "integer?",    0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_INT,   "finite?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_INT,   "real?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_INT,   "infinite?",   0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "((void)($r), SP_INT_NIL)" },  /* always nil (nullable int) */
  { TY_INT,   "abs2",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_int_mul($r, $r)" },  /* the Complex projection (#2328) */
  { TY_INT,   "real",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "imaginary",   0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_INT,   "imag",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "((void)($r), 0)" },
  { TY_INT,   "conj",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "conjugate",   0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "i",           0,   0, BF_ANY,      TY_COMPLEX,     BOPE_TEMPLATE, "((sp_Complex){0.0, (sp_float)($r), 0})" },
  { TY_INT,   "to_c",        0,   0, BF_ANY,      TY_COMPLEX,     BOPE_NONE },
  { TY_INT,   "arg",         0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },  /* Integer 0 or Float PI */
  { TY_INT,   "angle",       0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_INT,   "phase",       0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_INT,   "rect",        0,   0, BF_ANY,      TY_INT_ARRAY,   BOPE_TEMPLATE, "({ sp_IntArray *_t$t = sp_IntArray_new(); SP_GC_ROOT(_t$t); sp_IntArray_push(_t$t, ($r)); sp_IntArray_push(_t$t, 0); _t$t; })" },
  { TY_INT,   "rectangular", 0,   0, BF_ANY,      TY_INT_ARRAY,   BOPE_TEMPLATE, "({ sp_IntArray *_t$t = sp_IntArray_new(); SP_GC_ROOT(_t$t); sp_IntArray_push(_t$t, ($r)); sp_IntArray_push(_t$t, 0); _t$t; })" },
  { TY_INT,   "polar",       0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t); sp_PolyArray_push(_t$t, sp_box_int(($r) < 0 ? -($r) : ($r))); sp_PolyArray_push(_t$t, ($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0)); _t$t; })" },
  { TY_INT,   "ord",         0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "($r)" },
  { TY_INT,   "to_int",      0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "({ sp_int _t$t = ($r); _t$t == SP_INT_NIL ? 0 : _t$t; })" },
  { TY_INT,   "pred",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(($r) - 1)" },
  { TY_INT,   "succ",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(($r) + 1)" },
  { TY_INT,   "next",        0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(($r) + 1)" },
  { TY_INT,   "numerator",   0,   0, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "denominator", 0,   0, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "bit_length",  0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_int_bit_length($r)" },
  { TY_INT,   "magnitude",   0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(($r) < 0 ? -($r) : ($r))" },
  { TY_INT,   "nonzero?",    0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(($r) == 0 ? SP_INT_NIL : ($r))" },  /* self or nil (nullable int) */
  { TY_INT,   "ceildiv",     1, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "to_r",        0,   0, BF_ANY,      TY_RATIONAL,    BOPE_NONE },
  { TY_INT,   "rationalize", 0,   1, BF_ANY,      TY_RATIONAL,    BOPE_NONE },
  { TY_INT,   "times",       0, 127, BF_REQUIRED, TY_INT,         BOPE_NONE },  /* with a block: self */
  { TY_INT,   "upto",        0, 127, BF_REQUIRED, TY_INT,         BOPE_NONE },
  { TY_INT,   "downto",      0, 127, BF_REQUIRED, TY_INT,         BOPE_NONE },
  { TY_INT,   "step",        0, 127, BF_REQUIRED, TY_INT,         BOPE_NONE },
  { TY_INT,   "times",       0, 127, BF_NONE,     TY_RANGE,       BOPE_NONE },  /* without one: a range-like enumerator */
  { TY_INT,   "upto",        0, 127, BF_NONE,     TY_RANGE,       BOPE_NONE },
  { TY_INT,   "downto",      0, 127, BF_NONE,     TY_RANGE,       BOPE_NONE },
  { TY_INT,   "chr",         0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_int_chr($r)" },
  { TY_INT,   "chr",         0, 127, BF_ANY,      TY_STRING,      BOPE_NONE },
  { TY_INT,   "[]",          1,   2, BF_ANY,      TY_INT,         BOPE_NONE },  /* a bit, or a bit-range field */
  { TY_INT,   "fdiv",        1,   1, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "((sp_float)($r) / ($f0))" },
  { TY_INT,   "div",         1,   1, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "modulo",      1,   1, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "remainder",   1,   1, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "gcd",         0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "lcm",         0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { TY_INT,   "gcdlcm",      1,   1, BF_ANY,      TY_INT_ARRAY,   BOPE_NONE },
  { TY_INT,   "digits",      0,   0, BF_ANY,      TY_INT_ARRAY,   BOPE_TEMPLATE, "sp_int_digits($r, 10)" },
  { TY_INT,   "digits",      0, 127, BF_ANY,      TY_INT_ARRAY,   BOPE_NONE },  /* face-table fallback only */
  { TY_INT,   "to_s",        1,   1, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_int_to_s_base($r, $i0)" },
  /* Float: arms with no inference row of their own */
  { TY_FLOAT, "to_s",        0, 127, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_float_opt_to_s($r)" },
  { TY_FLOAT, "inspect",     0, 127, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_float_opt_inspect($r)" },
  { TY_FLOAT, "arg",         1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_FLOAT, "angle",       1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_FLOAT, "phase",       1, 127, BF_ANY,      TY_UNKNOWN,     BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_FLOAT, "arg",         0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },  /* Integer 0 or Float PI (#2316) */
  { TY_FLOAT, "angle",       0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_FLOAT, "phase",       0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))" },
  { TY_FLOAT, "to_c",        0,   0, BF_ANY,      TY_COMPLEX,     BOPE_NONE },
  { TY_FLOAT, "i",           0, 127, BF_ANY,      TY_COMPLEX,     BOPE_TEMPLATE, "((sp_Complex){0.0, ($r), 2})" },
  { TY_FLOAT, "coerce",      1,   1, BF_ANY,      TY_FLOAT_ARRAY, BOPE_NONE },  /* [Float(other), self] */
  { TY_FLOAT, "divmod",      1,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },  /* [Integer, Float] */
  { TY_FLOAT, "infinite?",   0, 127, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "(isinf($r) ? (($r) > 0 ? 1LL : -1LL) : SP_INT_NIL)" },  /* nil / 1 / -1 (nullable int) */
  { TY_FLOAT, "nan?",        0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(isnan($r) != 0)" },
  { TY_FLOAT, "finite?",     0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(isfinite($r) != 0)" },
  { TY_FLOAT, "positive?",   0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) > 0)" },
  { TY_FLOAT, "negative?",   0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) < 0)" },
  { TY_FLOAT, "zero?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(($r) == 0.0)" },
  { TY_FLOAT, "integer?",    0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void)($r), FALSE)" },
  { TY_FLOAT, "real?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void)($r), TRUE)" },
  { TY_FLOAT, "nonzero?",    0, 127, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "(($r) != 0.0 ? sp_box_float($r) : sp_box_nil())" },  /* self or nil */
  { TY_FLOAT, "div",         1,   1, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "({ sp_float _t$t = ($r); sp_float _t$u = $f0; if (_t$u == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\"); if (isinf(_t$t)) sp_raise_cls(\"FloatDomainError\", _t$t > 0 ? \"Infinity\" : \"-Infinity\"); if (isnan(_t$t)) sp_raise_cls(\"FloatDomainError\", \"NaN\"); sp_float_fit_i(floor(_t$t / _t$u)); })" },
  { TY_FLOAT, "abs2",        0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "(($r) * ($r))" },
  { TY_FLOAT, "real",        0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "($r)" },
  { TY_FLOAT, "conj",        0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "($r)" },
  { TY_FLOAT, "conjugate",   0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "($r)" },
  { TY_FLOAT, "next_float",  0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "nextafter($r, INFINITY)" },
  { TY_FLOAT, "prev_float",  0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "nextafter($r, -INFINITY)" },
  { TY_FLOAT, "abs",         0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "fabs($r)" },
  { TY_FLOAT, "magnitude",   0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "fabs($r)" },
  { TY_FLOAT, "modulo",      1,   1, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "sp_fmod($r, $e0)" },
  { TY_FLOAT, "modulo",      0, 127, BF_ANY,      TY_FLOAT,       BOPE_NONE },
  { TY_FLOAT, "remainder",   1,   1, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "sp_fremainder($r, $f0)" },
  { TY_FLOAT, "remainder",   0, 127, BF_ANY,      TY_FLOAT,       BOPE_NONE },
  { TY_FLOAT, "to_f",        0, 127, BF_ANY,      TY_FLOAT,       BOPE_TEMPLATE, "($r)" },
  { TY_FLOAT, "imag",        0, 127, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "((void)($r), (sp_int)0)" },
  { TY_FLOAT, "imaginary",   0, 127, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "((void)($r), (sp_int)0)" },
  { TY_FLOAT, "rect",        0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t); sp_PolyArray_push(_t$t, sp_box_float($r)); sp_PolyArray_push(_t$t, sp_box_int(0)); _t$t; })" },
  { TY_FLOAT, "rectangular", 0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t); sp_PolyArray_push(_t$t, sp_box_float($r)); sp_PolyArray_push(_t$t, sp_box_int(0)); _t$t; })" },
  { TY_FLOAT, "polar",       0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "({ sp_PolyArray *_t$t = sp_PolyArray_new(); SP_GC_ROOT(_t$t); sp_PolyArray_push(_t$t, sp_box_float(fabs($r))); sp_PolyArray_push(_t$t, ($r) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0)); _t$t; })" },
  { TY_FLOAT, "numerator",   0,   0, BF_ANY,      TY_POLY,        BOPE_TEMPLATE, "sp_float_to_rational($r).num" },  /* an Integer, or the non-finite Float (#3011) */
  { TY_FLOAT, "denominator", 0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_float_to_rational($r).den" },
  { TY_FLOAT, "to_r",        0,   0, BF_ANY,      TY_RATIONAL,    BOPE_TEMPLATE, "sp_float_to_rational($r)" },
  { TY_FLOAT, "rationalize", 0,   0, BF_ANY,      TY_RATIONAL,    BOPE_TEMPLATE, "sp_float_rationalize0($r)" },
  { TY_FLOAT, "rationalize", 0,   1, BF_ANY,      TY_RATIONAL,    BOPE_NONE },
  { TY_FLOAT, "eql?",        1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },

  /* String: the calls typed by name, arity and block form. Promote mode's
     to_i, casecmp (its operand), unpack1 (the format literal), byteindex
     (the needle), each_line and lines (the separator), scan (the pattern)
     and gsub (a blockless regexp literal) stay in infer_call_inner. */
  { TY_STRING, "clear",           0,   0, BF_ANY,      TY_STRING,     BOPE_NONE },  /* self (#2332) */
  { TY_STRING, "[]=",             2,   3, BF_ANY,      TY_STRING,     BOPE_NONE },  /* the assigned string (#2370) */
  { TY_STRING, "clone",           1,   1, BF_ANY,      TY_STRING,     BOPE_NONE },  /* clone(freeze: ...) */
  { TY_STRING, "encoding",        0,   0, BF_ANY,      TY_POLY,       BOPE_TEMPLATE, "sp_box_encoding(sp_str_is_binary($r) ? sp_encoding_binary() : sp_encoding_utf8())", 0 },  /* an Encoding value */
  { TY_STRING, "upcase",          0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_upcase($r)", 0 },
  { TY_STRING, "upcase",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "downcase",        0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_downcase($r)", 0 },
  { TY_STRING, "downcase",        0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "capitalize",      0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_capitalize($r)", 0 },
  { TY_STRING, "capitalize",      0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "swapcase",        0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_swapcase($r)", 0 },
  { TY_STRING, "swapcase",        0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "reverse",         0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_reverse($r)", 0 },
  { TY_STRING, "delete_prefix",   1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_delete_prefix($r, $s0)", 0 },
  { TY_STRING, "delete_suffix",   1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_delete_suffix($r, $s0)", 0 },
  { TY_STRING, "strip",           0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_strip($r)", 0 },
  { TY_STRING, "lstrip",          0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_lstrip($r)", 0 },
  { TY_STRING, "rstrip",          0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_rstrip($r)", 0 },
  { TY_STRING, "chomp",           0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_chomp($r)", 0 },
  { TY_STRING, "chomp",           2, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_chomp($r)", 0 },
  { TY_STRING, "chomp",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "chop",            0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_chop($r)", 0 },
  { TY_STRING, "chr",             0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_chr($r)", 0 },
  { TY_STRING, "chr",             0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "clamp",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "squeeze",         2, 127, BF_ANY,      TY_STRING,     BOPE_STR_SET_N, NULL, 0 },
  { TY_STRING, "squeeze",         0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_squeeze($r)", 0 },
  { TY_STRING, "squeeze",         1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_squeeze_chars($r, $s0)", 0 },
  { TY_STRING, "squeeze",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "tr",              2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_tr($r, $s0, $s1)", 0 },
  { TY_STRING, "tr",              0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "tr_s",            2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_tr_s($r, $s0, $s1)", 0 },
  { TY_STRING, "tr_s",            0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "succ",            0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_succ($r)", 0 },
  { TY_STRING, "succ",            0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "next",            0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_succ($r)", 0 },
  { TY_STRING, "next",            0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "delete",          2, 127, BF_ANY,      TY_STRING,     BOPE_STR_SET_N, NULL, 0 },
  { TY_STRING, "delete",          1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_delete($r, $s0)", 0 },
  { TY_STRING, "delete",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "slice!",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },  /* removed part, or nil */
  { TY_STRING, "[]",              0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "slice",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "byteslice",       2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_byteslice($r, $i0, $i1)", 0 },
  { TY_STRING, "byteslice",       1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ sp_Range _t$t = sp_range_ix($e0); sp_str_byteslice_range($r, _t$t.first, _t$t.last, _t$t.excl, _t$t.first == INTPTR_MIN, _t$t.last == INTPTR_MAX); })", BOP_K(TY_RANGE) },
  { TY_STRING, "byteslice",       1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_byteslice1($r, $i0)", 0 },
  { TY_STRING, "byteslice",       0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "bytesplice",      0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "append_as_bytes", 0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "force_encoding",  0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "b",               0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_b($r)", 0 },
  { TY_STRING, "b",               0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "encode",          0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "($r)", 0 },
  { TY_STRING, "encode",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "encode!",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "dump",            0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_dump($r)", 0 },
  { TY_STRING, "undump",          0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_undump($r)", 0 },
  { TY_STRING, "scrub",           0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_scrub($r, 0)", 0 },
  { TY_STRING, "scrub",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "scrub!",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "crypt",           1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_crypt($r, $s0)", 0 },
  { TY_STRING, "crypt",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  /* index: a String or a Regexp needle, both a nullable int (SP_INT_NIL) */
  { TY_STRING, "index",           0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "rindex",          0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "to_i",            0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "count",           2, 127, BF_ANY,      TY_INT,        BOPE_STR_SET_N, NULL, 0 },
  { TY_STRING, "count",           1,   1, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_count($r, $s0)", 0 },
  { TY_STRING, "count",           0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "oct",             0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_oct($r)", 0 },
  { TY_STRING, "oct",             0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "hex",             0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_to_i_base($r, 16)", 0 },
  { TY_STRING, "hex",             0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "ord",             0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_ord($r)", 0 },
  { TY_STRING, "ord",             0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "bytesize",        0, 127, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "sp_str_bytesize_m($r)", 0 },
  { TY_STRING, "setbyte",         0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "getbyte",         0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "sum",             0,   0, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_int _t$u = 16; sp_str_sum_bits(_t$t, _t$u); })", 0 },
  { TY_STRING, "sum",             1,   1, BF_ANY,      TY_INT,        BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_int _t$u = $i0; sp_str_sum_bits(_t$t, _t$u); })", 0 },
  { TY_STRING, "sum",             0,   1, BF_ANY,      TY_INT,        BOPE_NONE },
  { TY_STRING, "partition",       0, 127, BF_ANY,      TY_STR_ARRAY,  BOPE_NONE },
  { TY_STRING, "rpartition",      0, 127, BF_ANY,      TY_STR_ARRAY,  BOPE_NONE },
  /* the value-form mutators: the post-mutation string; the no-change bang
     contract carries nil as NULL through the nullable string. gsub! with a
     pattern alone and no block is an Enumerator of the matches, as gsub's. */
  { TY_STRING, "gsub!",           1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { TY_STRING, "gsub!",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "sub!",            0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "upcase!",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "downcase!",       0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "capitalize!",     0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "swapcase!",       0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "strip!",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "lstrip!",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "rstrip!",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "chomp!",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "chop!",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "squeeze!",        0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "tr!",             0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "delete!",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "reverse!",        0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "tr_s!",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "delete_prefix!",  0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "delete_suffix!",  0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "dedup",           0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_uminus_val($r)", 0 },
  { TY_STRING, "dedup",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "succ!",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "next!",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  /* emit_call_body's arms ahead of the scalar chain: concat() (stage 1) and
     nil? (stage 2), each looked up where its arm sat */
  { TY_STRING, "concat",          0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_str_check_mutable(_t$t); _t$t; })", 0, 0, 0, 1 },
  { TY_STRING, "nil?",            0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(($r) == 0)", 0, 0, 0, 2 },  /* a nullable String carries nil as NULL */
  { TY_STRING, "concat",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },  /* self (#2309) */
  { TY_STRING, "<<",              0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "prepend",         0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_str_check_mutable(_t$t); _t$t; })", 0 },
  { TY_STRING, "prepend",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "insert",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "replace",         0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "ascii_only?",     0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_ascii_only($r)", 0 },
  { TY_STRING, "ascii_only?",     0, 127, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { TY_STRING, "valid_encoding?", 0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_valid_encoding($r)", 0 },
  { TY_STRING, "valid_encoding?", 0, 127, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { TY_STRING, "to_f",            0,   0, BF_ANY,      TY_FLOAT,      BOPE_TEMPLATE, "sp_str_to_f_cruby($r)", 0 },
  { TY_STRING, "to_f",            0, 127, BF_ANY,      TY_FLOAT,      BOPE_NONE },
  { TY_STRING, "to_r",            0,   0, BF_ANY,      TY_RATIONAL,   BOPE_TEMPLATE, "sp_str_to_r($r)", 0 },
  { TY_STRING, "to_c",            0,   0, BF_ANY,      TY_COMPLEX,    BOPE_TEMPLATE, "sp_str_to_c($r)", 0 },
  { TY_STRING, "intern",          1, 127, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_sym_intern_n(_t$t, sp_str_byte_len(_t$t)); })", 0 },
  { TY_STRING, "intern",          0,   0, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_sym_intern_n(_t$t, sp_str_byte_len(_t$t)); })", 0 },
  /* the iterators: blockless with no argument an Enumerator; with a block
     they iterate and answer the receiver */
  { TY_STRING, "each_char",       0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { TY_STRING, "each_char",       0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "each_byte",       0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { TY_STRING, "each_byte",       0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "each_codepoint",  0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { TY_STRING, "chars",           0,   0, BF_NONE,     TY_STR_ARRAY,  BOPE_TEMPLATE, "sp_str_chars($r)", 0 },
  { TY_STRING, "chars",           0, 127, BF_REQUIRED, TY_STRING,     BOPE_NONE },
  { TY_STRING, "chars",           0, 127, BF_ANY,      TY_STR_ARRAY,  BOPE_NONE },
  { TY_STRING, "bytes",           0,   0, BF_NONE,     TY_INT_ARRAY,  BOPE_TEMPLATE, "sp_str_bytes($r)", 0 },
  { TY_STRING, "bytes",           0, 127, BF_REQUIRED, TY_STRING,     BOPE_NONE },
  { TY_STRING, "bytes",           0, 127, BF_ANY,      TY_INT_ARRAY,  BOPE_NONE },
  { TY_STRING, "codepoints",      0,   0, BF_NONE,     TY_INT_ARRAY,  BOPE_TEMPLATE, "sp_str_codepoints($r)", 0 },
  { TY_STRING, "codepoints",      0, 127, BF_REQUIRED, TY_STRING,     BOPE_NONE },
  { TY_STRING, "codepoints",      0, 127, BF_ANY,      TY_INT_ARRAY,  BOPE_NONE },
  { TY_STRING, "split",           0,   0, BF_NONE,     TY_STR_ARRAY,  BOPE_TEMPLATE, "sp_str_split_ws($r)", 0 },
  { TY_STRING, "split",           0, 127, BF_REQUIRED, TY_STRING,     BOPE_NONE },
  { TY_STRING, "split",           0, 127, BF_ANY,      TY_STR_ARRAY,  BOPE_NONE },
  { TY_STRING, "upto",            1,   1, BF_ANY,      TY_STR_ARRAY,  BOPE_NONE },  /* blockless: the materialized sequence */
  { TY_STRING, "unpack",          1,   1, BF_ANY,      TY_POLY_ARRAY, BOPE_TEMPLATE, "sp_str_unpack($r, $s0)", 0 },
  { TY_STRING, "unpack",          1,   2, BF_ANY,      TY_POLY_ARRAY, BOPE_NONE },
  { TY_STRING, "sub",             2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_sub_str_str_hash($r, $e0, $e1)", 0, BOP_K(TY_STR_STR_HASH) },
  { TY_STRING, "sub",             2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_sub($r, $s0, $s1)", 0 },
  { TY_STRING, "sub",             0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "center",          1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_center($r, $i0)", 0 },
  { TY_STRING, "center",          2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_center2($r, $i0, $s1)", 0 },
  { TY_STRING, "center",          0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "ljust",           1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_ljust($r, $i0)", 0 },
  { TY_STRING, "ljust",           2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_ljust2($r, $i0, $s1)", 0 },
  { TY_STRING, "ljust",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "rjust",           1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_rjust($r, $i0)", 0 },
  { TY_STRING, "rjust",           2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_rjust2($r, $i0, $s1)", 0 },
  { TY_STRING, "rjust",           0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "*",               0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { TY_STRING, "to_sym",          0, 127, BF_ANY,      TY_SYMBOL,     BOPE_TEMPLATE, "({ const char *_t$t = $r; sp_sym_intern_n(_t$t, sp_str_byte_len(_t$t)); })", 0 },
  { TY_STRING, "to_s",            0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ const char *_t$t = $r; _t$t ? _t$t : sp_str_frozen_empty; })", 0 },
  { TY_STRING, "to_str",          0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ const char *_t$t = $r; if (!_t$t) sp_nil_recv(\"to_str\"); _t$t; })", 0 },
  { TY_STRING, "empty?",          0, 127, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_empty_p($r)", 0 },
  { TY_STRING, "include?",        1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_include($r, $s0)", 0 },
  { TY_STRING, "start_with?",     2, 127, BF_ANY,      TY_BOOL,       BOPE_STR_AFFIX_ANY, NULL, 0 },
  { TY_STRING, "start_with?",     0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (sp_bool)0)", 0 },
  { TY_STRING, "end_with?",       2, 127, BF_ANY,      TY_BOOL,       BOPE_STR_AFFIX_ANY, NULL, 0 },
  { TY_STRING, "end_with?",       0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "((void)($r), (sp_bool)0)", 0 },
  { TY_STRING, "end_with?",       1,   1, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "sp_str_end_with($r, $s0)", 0 },
  { TY_STRING, "=~",              1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), sp_raise_cls(\"TypeError\", \"type mismatch: String given\"), (sp_bool)0)", BOP_K(TY_STRING) },
  { TY_STRING, "=~",              1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), sp_raise_cls(\"NoMethodError\", ($e0) ? \"undefined method '=~' for true\" : \"undefined method '=~' for false\"), (sp_bool)0)", BOP_K(TY_BOOL) },
  { TY_STRING, "!~",              1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), sp_raise_cls(\"TypeError\", \"type mismatch: String given\"), (sp_bool)0)", BOP_K(TY_STRING) },
  { TY_STRING, "!~",              1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "((void)($r), sp_raise_cls(\"NoMethodError\", ($e0) ? \"undefined method '=~' for true\" : \"undefined method '=~' for false\"), (sp_bool)0)", BOP_K(TY_BOOL) },
  { TY_STRING, "casecmp",         1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "({ const char *_t$t = $r; SP_GC_ROOT_STR(_t$t); sp_RbVal _t$u = $e0; const char *_t$v = sp_poly_check_str(_t$u); (_t$v || _t$u.tag == SP_TAG_STR) ? sp_box_int(sp_str_casecmp(_t$t, _t$v ? _t$v : \"\")) : sp_box_nil(); })", BOP_K(TY_POLY) },
  { TY_STRING, "casecmp?",        1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "({ const char *_t$t = $r; SP_GC_ROOT_STR(_t$t); sp_RbVal _t$u = $e0; const char *_t$v = sp_poly_check_str(_t$u); (_t$v || _t$u.tag == SP_TAG_STR) ? sp_box_bool(sp_str_casecmp(_t$t, _t$v ? _t$v : \"\") == 0) : sp_box_nil(); })", BOP_K(TY_POLY) },
  { TY_STRING, "casecmp",         1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "sp_str_casecmp($r, $e0)", BOP_K(TY_STRING) | BOP_K(TY_UNKNOWN) },
  { TY_STRING, "casecmp?",        1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "(sp_str_casecmp($r, $e0) == 0)", BOP_K(TY_STRING) | BOP_K(TY_UNKNOWN) },
  { TY_STRING, "lines",           0,   0, BF_NONE,     TY_STR_ARRAY,  BOPE_TEMPLATE, "sp_str_lines($r)", 0 },
  { TY_STRING, "lines",           0,   0, BF_REQUIRED, TY_STRING,     BOPE_TEMPLATE, "sp_str_lines($r)", 0 },  /* the block form iterates and answers the receiver */
  { TY_STRING, "lines",           1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "sp_str_lines_sep($r, $e0)", BOP_K(TY_STRING) },
  { TY_STRING, "gsub",            2,   2, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_str_gsub($r, $s0, $s1)", 0 },
  { TY_STRING, "inspect",         0, 127, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "({ const char *_t$t = $r; _t$t ? sp_str_inspect(_t$t) : SPL(\"nil\"); })", 0 },

  /* Hash, any kind (BOP_ANY_HASH): the result kinds read off the name,
     arity and block form, some derived from the receiver's kind. []=/store,
     fetch with a default or a block, dig, a blockless sum(init),
     map/collect with a block, transform_keys/values, merge(other),
     replace and merge!/update(other) read the operands or the block, and
     stay in infer_hash_call.

     The codegen rows are emit_hash_call's arms that read only the
     receiver's variant ($H, $K), the receiver and the arguments, looked up
     at the head of its arms; the ones that branch on the variant or on an
     operand's kind are custom emitters (codegen_call_hash.c). A codegen
     row narrower than the inference row of its name answers what that row
     does; one with no inference row of its own answers what the rules
     after the lookup answered for it (all of these names are in Hash's
     builtin table, so no Object reopen answers them). replace, default=
     (typed from the operand ahead of the lookup) and to_a/entries with a
     block carry none. */
#define HASH_OR_ARRAY (BOP_K(TY_STR_INT_HASH) | BOP_K(TY_STR_STR_HASH) | BOP_K(TY_INT_INT_HASH) | \
                       BOP_K(TY_INT_STR_HASH) | BOP_K(TY_SYM_POLY_HASH) | BOP_K(TY_STR_POLY_HASH) | \
                       BOP_K(TY_POLY_POLY_HASH) | BOP_K(TY_INT_ARRAY) | BOP_K(TY_FLOAT_ARRAY) | \
                       BOP_K(TY_STR_ARRAY) | BOP_K(TY_POLY_ARRAY) | BOP_K(TY_INT_ARRAY_ARRAY) | \
                       BOP_K(TY_FLOAT_ARRAY_ARRAY))
  { BOP_ANY_HASH, "compare_by_identity?", 0, 0, BF_ANY,  TY_BOOL,        BOPE_TEMPLATE, "((void)($r), 0)" },  /* always false; the receiver still runs */
  { BOP_ANY_HASH, "equal?",           1,   1, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "((void *)($r) == (void *)($e0))", HASH_OR_ARRAY },  /* pointer identity */
  { BOP_ANY_HASH, "equal?",           1,   1, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "0" },
  { BOP_ANY_HASH, "length",           0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_$HHash_length($r)" },
  { BOP_ANY_HASH, "size",             0,   0, BF_ANY,      TY_INT,         BOPE_TEMPLATE, "sp_$HHash_length($r)" },
  { BOP_ANY_HASH, "count",            0,   0, BF_NONE,     TY_INT,         BOPE_TEMPLATE, "sp_$HHash_length($r)" },
  { BOP_ANY_HASH, "count",            1,   1, BF_NONE,     TY_INT,         BOPE_HASH_PATTERN },  /* the pairs == the argument */
  { BOP_ANY_HASH, "empty?",           0,   0, BF_ANY,      TY_BOOL,        BOPE_TEMPLATE, "(sp_$HHash_length($r) == 0)" },
  /* blockless predicates fold on the pair count (a pair is always truthy,
     so all? is unconditionally true); with a pattern, each pair is tested */
  { BOP_ANY_HASH, "any?",             0,   0, BF_NONE,     TY_BOOL,        BOPE_TEMPLATE, "({ sp_$HHash *_t$t = $r; (_t$t && _t$t->len > 0); })" },
  { BOP_ANY_HASH, "none?",            0,   0, BF_NONE,     TY_BOOL,        BOPE_TEMPLATE, "({ sp_$HHash *_t$t = $r; (!_t$t || _t$t->len == 0); })" },
  { BOP_ANY_HASH, "all?",             0,   0, BF_NONE,     TY_BOOL,        BOPE_TEMPLATE, "({ sp_$HHash *_t$t = $r; (void)_t$t; 1; })" },
  { BOP_ANY_HASH, "one?",             0,   0, BF_NONE,     TY_BOOL,        BOPE_TEMPLATE, "({ sp_$HHash * _t$t = $r; sp_$HHash_length(_t$t) == 1; })" },  /* #2354 */
  { BOP_ANY_HASH, "any?",             1,   1, BF_NONE,     TY_BOOL,        BOPE_HASH_PATTERN },
  { BOP_ANY_HASH, "none?",            1,   1, BF_NONE,     TY_BOOL,        BOPE_HASH_PATTERN },
  { BOP_ANY_HASH, "one?",             1,   1, BF_NONE,     TY_BOOL,        BOPE_HASH_PATTERN },
  { BOP_ANY_HASH, "all?",             1,   1, BF_NONE,     TY_BOOL,        BOPE_HASH_PATTERN_ALL },
  /* the readers and copies */
  { BOP_ANY_HASH, "[]",               1,   1, BF_ANY,      BOPR_HASH_VAL,  BOPE_HASH_AREF },
  { BOP_ANY_HASH, "fetch",            1,   1, BF_NONE,     BOPR_HASH_VAL,  BOPE_HASH_FETCH },  /* KeyError on a miss */
  { BOP_ANY_HASH, "has_key?",         1,   1, BF_ANY,      TY_BOOL,        BOPE_HASH_HAS_KEY },
  { BOP_ANY_HASH, "key?",             1,   1, BF_ANY,      TY_BOOL,        BOPE_HASH_HAS_KEY },
  { BOP_ANY_HASH, "include?",         1,   1, BF_ANY,      TY_BOOL,        BOPE_HASH_HAS_KEY },
  { BOP_ANY_HASH, "member?",          1,   1, BF_ANY,      TY_BOOL,        BOPE_HASH_HAS_KEY },
  { BOP_ANY_HASH, "key",              1,   1, BF_ANY,      BOPR_HASH_KEY_OF, BOPE_HASH_KEY },
  { BOP_ANY_HASH, "keys",             0,   0, BF_ANY,      BOPR_HASH_KEYS, BOPE_HASH_KEYS },
  { BOP_ANY_HASH, "values",           0,   0, BF_ANY,      BOPR_HASH_VALS, BOPE_TEMPLATE, "sp_$HHash_values($r)" },
  { BOP_ANY_HASH, "values_at",        0,   0, BF_ANY,      TY_POLY_ARRAY,  BOPE_TEMPLATE, "((void)($r), sp_PolyArray_new())" },  /* #2408 */
  { BOP_ANY_HASH, "inspect",          0,   0, BF_ANY,      TY_STRING,      BOPE_TEMPLATE, "sp_$HHash_inspect($r)" },
  { BOP_ANY_HASH, "to_s",             0,   0, BF_ANY,      TY_STRING,      BOPE_HASH_TO_S },
  { BOP_ANY_HASH, "to_proc",          0,   0, BF_ANY,      TY_PROC,        BOPE_HASH_TO_PROC },  /* a lambda over the hash */
  { BOP_ANY_HASH, "default_proc",     0,   0, BF_NONE,     TY_PROC,        BOPE_HASH_DEFAULT_PROC },
  { BOP_ANY_HASH, "dup",              0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "sp_$HHash_dup($r)" },
  { BOP_ANY_HASH, "clone",            0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "({ sp_$HHash *_t$t = $r; sp_$HHash *_t$u = sp_$HHash_dup(_t$t); if (_t$t && sp_gc_is_frozen(_t$t)) sp_gc_freeze(_t$u); _t$u; })" },  /* the frozen flag too (#3751) */
  { BOP_ANY_HASH, "merge",            0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "sp_$HHash_dup($r)" },  /* a copy (#2340) */
  { BOP_ANY_HASH, "slice",            0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "({ (void)($r); sp_$HHash_new(); })" },  /* an empty hash (#2349) */
  /* the mutators */
  { BOP_ANY_HASH, "delete",           1,   1, BF_ANY,      BOPR_HASH_VAL,  BOPE_HASH_DELETE },  /* the deleted value, or nil */
  { BOP_ANY_HASH, "shift",            0,   0, BF_NONE,     TY_POLY,        BOPE_HASH_SHIFT },  /* the first pair, or nil */
  { BOP_ANY_HASH, "replace",          1,   1, BF_ANY,      TY_UNKNOWN,     BOPE_HASH_REPLACE },
  { BOP_ANY_HASH, "default=",         1,   1, BF_ANY,      TY_UNKNOWN,     BOPE_HASH_SET_DEFAULT },
  { BOP_ANY_HASH, "merge!",           2, 127, BF_NONE,     BOPR_SELF,      BOPE_HASH_MERGE_BANG_MANY },  /* #2431 */
  { BOP_ANY_HASH, "update",           2, 127, BF_NONE,     BOPR_SELF,      BOPE_HASH_MERGE_BANG_MANY },
  /* the conversions */
  { BOP_ANY_HASH, "to_a",             0,   0, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_TO_A },  /* the [key, value] pairs */
  { BOP_ANY_HASH, "to_a",             0,   0, BF_REQUIRED, TY_UNKNOWN,     BOPE_HASH_TO_A },
  { BOP_ANY_HASH, "entries",          0,   0, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_TO_A },
  { BOP_ANY_HASH, "entries",          0,   0, BF_REQUIRED, TY_UNKNOWN,     BOPE_HASH_TO_A },
  { BOP_ANY_HASH, "sort",             0,   0, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_SORT },  /* by Array#<=> over the pairs */
  { BOP_ANY_HASH, "invert",           0,   0, BF_ANY,      BOPR_HASH_INVERT, BOPE_HASH_INVERT },
  { BOP_ANY_HASH, "each",            0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },  /* blockless: an external Enumerator over the pairs */
  { BOP_ANY_HASH, "each_pair",        0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "each_key",         0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "each_value",       0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "each_with_index",  0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "map",              0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "collect",          0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "select",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "filter",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "reject",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "find",             0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "detect",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "find_all",         0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "flat_map",         0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "filter_map",       0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "sort_by",          0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "min_by",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "max_by",           0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "group_by",         0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "partition",        0,   0, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },
  { BOP_ANY_HASH, "any?",             0,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "all?",             0,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "none?",            0,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "one?",             0,   1, BF_NONE,     TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "any?",             0, 127, BF_REQUIRED, TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "all?",             0, 127, BF_REQUIRED, TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "deconstruct_keys", 1,   1, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "((void)($b0), $r)" },  /* the hash itself */
  { BOP_ANY_HASH, "compact!",         0,   0, BF_ANY,      TY_POLY,        BOPE_HASH_COMPACT_BANG },  /* self or nil */
  { BOP_ANY_HASH, "chunk",            0, 127, BF_REQUIRED, TY_ENUMERATOR,  BOPE_NONE },  /* of [key, [[k, v], ...]] pairs */
  { BOP_ANY_HASH, "to_proc",          0, 127, BF_ANY,      TY_PROC,        BOPE_NONE },
  { BOP_ANY_HASH, "first",            0,   0, BF_NONE,     TY_POLY,        BOPE_HASH_FIRST },  /* Enumerable's, over the [key, value] pairs */
  { BOP_ANY_HASH, "first",            1,   1, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_TAKE },
  { BOP_ANY_HASH, "take",             1,   1, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_TAKE },
  { BOP_ANY_HASH, "drop",             1,   1, BF_NONE,     TY_POLY_ARRAY,  BOPE_HASH_DROP },
  { BOP_ANY_HASH, "to_h",             0,   0, BF_NONE,     BOPR_SELF,      BOPE_NONE },  /* identity */
  { BOP_ANY_HASH, "slice",            0, 127, BF_ANY,      BOPR_SELF,      BOPE_NONE },  /* a copy, or a key subset */
  { BOP_ANY_HASH, "except",           0, 127, BF_ANY,      BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "dup",              0, 127, BF_ANY,      BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "clone",            0, 127, BF_ANY,      BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "[]",               0, 127, BF_ANY,      BOPR_HASH_VAL,  BOPE_NONE },
  { BOP_ANY_HASH, "delete",           0, 127, BF_ANY,      BOPR_HASH_VAL,  BOPE_NONE },
  { BOP_ANY_HASH, "default",          0,   1, BF_ANY,      TY_POLY,        BOPE_HASH_DEFAULT },  /* default(key) too (#2409) */
  { BOP_ANY_HASH, "length",           0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { BOP_ANY_HASH, "size",             0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { BOP_ANY_HASH, "count",            0, 127, BF_ANY,      TY_INT,         BOPE_NONE },
  { BOP_ANY_HASH, "<",                1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },  /* subset / superset */
  { BOP_ANY_HASH, "<=",               1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, ">",                1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, ">=",               1,   1, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "keys",             0, 127, BF_ANY,      BOPR_HASH_KEYS, BOPE_NONE },
  { BOP_ANY_HASH, "values",           0, 127, BF_ANY,      BOPR_HASH_VALS, BOPE_NONE },
  { BOP_ANY_HASH, "values_at",        0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "fetch_values",     0, 127, BF_ANY,      TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "to_a",             0, 127, BF_NONE,     TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "entries",          0, 127, BF_NONE,     TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "sort",             0, 127, BF_NONE,     TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "find",             0, 127, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },  /* the winning [k, v] pair, or nil */
  { BOP_ANY_HASH, "detect",           0, 127, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },
  { BOP_ANY_HASH, "sort_by",          0, 127, BF_REQUIRED, TY_POLY_ARRAY,  BOPE_NONE },  /* pairs ordered by the block value */
  { BOP_ANY_HASH, "sum",              0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },  /* boxed accumulation */
  { BOP_ANY_HASH, "sum",              0,   0, BF_NONE,     TY_INT,         BOPE_TEMPLATE, "({ sp_$HHash * _t$t = $r; sp_$HHash_length(_t$t) == 0 ? (sp_int)(0) : (sp_raise_cls(\"TypeError\", \"Array can't be coerced into Integer\"), (sp_int)0); })" },  /* 0 + [k, v] is a TypeError */
  { BOP_ANY_HASH, "select!",          0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },  /* self, or nil when nothing was removed */
  { BOP_ANY_HASH, "filter!",          0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },
  { BOP_ANY_HASH, "reject!",          0, 127, BF_REQUIRED, TY_POLY,        BOPE_NONE },
  { BOP_ANY_HASH, "keep_if",          0, 127, BF_REQUIRED, BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "delete_if",        0, 127, BF_REQUIRED, BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "select",           0, 127, BF_REQUIRED, BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "filter",           0, 127, BF_REQUIRED, BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "reject",           0, 127, BF_REQUIRED, BOPR_SELF,      BOPE_NONE },
  { BOP_ANY_HASH, "clear",            0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "({ sp_$HHash * _t$t = $r; if (sp_gc_is_frozen(_t$t)) sp_raise_frozen_hash_at(_t$t, $K); sp_$HHash_clear(_t$t); _t$t; })" },  /* #2340/#2349/#2351, #3001 */
  { BOP_ANY_HASH, "to_hash",          0,   0, BF_ANY,      BOPR_SELF,      BOPE_TEMPLATE, "$r" },  /* the receiver itself, not a copy */
  { BOP_ANY_HASH, "rehash",           0,   0, BF_ANY,      BOPR_SELF,      BOPE_HASH_REHASH },
  { BOP_ANY_HASH, "compact",          0,   0, BF_ANY,      BOPR_SELF,      BOPE_HASH_COMPACT },
  { BOP_ANY_HASH, "shift",            0,   0, BF_ANY,      TY_POLY,        BOPE_NONE },  /* a [key, value] pair, or nil (#2349) */
  { BOP_ANY_HASH, "has_key?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "key?",             0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "include?",         0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "member?",          0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "has_value?",       0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "value?",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "empty?",           0, 127, BF_ANY,      TY_BOOL,        BOPE_NONE },
  { BOP_ANY_HASH, "each_with_object", 1, 127, BF_NONE,     TY_ENUMERATOR,  BOPE_NONE },  /* #2540 */
  { BOP_ANY_HASH, "flatten",          0,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_HASH_FLATTEN },
  { BOP_ANY_HASH, "assoc",            1,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_HASH_ASSOC },
  { BOP_ANY_HASH, "rassoc",           1,   1, BF_ANY,      TY_POLY_ARRAY,  BOPE_HASH_ASSOC },

  /* Array, any kind (BOP_ANY_ARRAY): the result kinds read off the name,
     arity and block form, some derived from the receiver's kind. The rules
     that read the operands, the block body, the receiver node or the
     receiver's exact kind stay in infer_array_call (analyze_infer_recv.c). */
  { BOP_ANY_ARRAY, "each",                  0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },  /* blockless: an external Enumerator */
  { BOP_ANY_ARRAY, "reverse_each",          0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { BOP_ANY_ARRAY, "each_entry",            0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { BOP_ANY_ARRAY, "each_with_index",       0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { BOP_ANY_ARRAY, "each_index",            0,   0, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { BOP_ANY_ARRAY, "each_slice",            1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },  /* a materialized Enumerator */
  { BOP_ANY_ARRAY, "each_cons",             1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },
  { BOP_ANY_ARRAY, "cycle",                 1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_ARRAY_CYCLE_N },
  { BOP_ANY_ARRAY, "slice_before",          1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_ARRAY_SLICE_GROUPS, 0, 0, 0, 0, 3 },  /* stage 3: emit_blockless_enumerator */
  { BOP_ANY_ARRAY, "slice_after",           1,   1, BF_NONE,     TY_ENUMERATOR, BOPE_ARRAY_SLICE_GROUPS, 0, 0, 0, 0, 3 },
  { BOP_ANY_ARRAY, "each_with_object",      1, 127, BF_NONE,     TY_ENUMERATOR, BOPE_NONE },  /* #2540 */
  { BOP_ANY_ARRAY, "chunk",                 0, 127, BF_REQUIRED, TY_ENUMERATOR, BOPE_NONE },  /* [key, run] pairs */
  { BOP_ANY_ARRAY, "select",                0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "reject",                0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "filter",                0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "find_all",              0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "sort_by",               0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "sort_by!",              0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "find",                  1, 127, BF_REQUIRED, TY_POLY,       BOPE_NONE },  /* find(ifnone): the element or the proc's value */
  { BOP_ANY_ARRAY, "detect",                1, 127, BF_REQUIRED, TY_POLY,       BOPE_NONE },
  { BOP_ANY_ARRAY, "find",                  0,   0, BF_REQUIRED, BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "detect",                0,   0, BF_REQUIRED, BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "bsearch",               0, 127, BF_REQUIRED, BOPR_ELEM,     BOPE_NONE },  /* element or nil */
  { BOP_ANY_ARRAY, "bsearch_index",         0, 127, BF_REQUIRED, TY_INT,        BOPE_NONE },  /* index, or nil */
  { BOP_ANY_ARRAY, "at",                    1,   1, BF_ANY,      BOPR_ELEM,     BOPE_NONE },  /* like [i] */
  { BOP_ANY_ARRAY, "length",                0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { BOP_ANY_ARRAY, "size",                  0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { BOP_ANY_ARRAY, "count",                 0, 127, BF_ANY,      TY_INT,        BOPE_NONE },
  { BOP_ANY_ARRAY, "first",                 1,   1, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_int _t$t = $i0; if (_t$t < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_$AArray_slice($r, 0, _t$t); })" },  /* first(n)/last(n): a subarray */
  { BOP_ANY_ARRAY, "last",                  1,   1, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_int _t$t = $i0; if (_t$t < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_$AArray_slice($r, -_t$t, _t$t); })" },
  { BOP_ANY_ARRAY, "first",                 0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "last",                  0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "min",                   1,   1, BF_NONE,     BOPR_SELF,     BOPE_ARRAY_NMIN },  /* min(n)/max(n): a subarray */
  { BOP_ANY_ARRAY, "max",                   1,   1, BF_NONE,     BOPR_SELF,     BOPE_ARRAY_NMIN },
  { BOP_ANY_ARRAY, "min",                   0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "max",                   0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "pop",                   1,   1, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_SHIFT_N },  /* pop(n)/shift(n): the removed subarray */
  { BOP_ANY_ARRAY, "shift",                 1,   1, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_SHIFT_N },
  { BOP_ANY_ARRAY, "pop",                   0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "shift",                 0, 127, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "slice",                 2,   2, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $gsp_int _t$u = $i0; sp_int _t$v = $i1; sp_int _t$w = sp_$AArray_length(_t$t); (_t$v < 0 || _t$u > _t$w || _t$u < -_t$w) ? (sp_$AArray *)0 : sp_$AArray_slice(_t$t, _t$u, _t$v); })" },  /* nil for a negative length or a start outside [-len, len] */
  { BOP_ANY_ARRAY, "slice!",                2,   2, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_slice_bang($h, $i0, $i1)" },  /* the removed subarray */
  { BOP_ANY_ARRAY, "cycle",                 0, 127, BF_REQUIRED, TY_NIL,        BOPE_NONE },  /* the block form returns nil */
  { BOP_ANY_ARRAY, "minmax",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },  /* [min, max], same element kind */
  { BOP_ANY_ARRAY, "join",                  0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { BOP_ANY_ARRAY, "pack",                  1,   1, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_$AArray_pack($h, $s0)" },
  { BOP_ANY_ARRAY, "inspect",               0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { BOP_ANY_ARRAY, "to_s",                  0, 127, BF_ANY,      TY_STRING,     BOPE_NONE },
  { BOP_ANY_ARRAY, "empty?",                0, 127, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "include?",              0, 127, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "frozen?",               0, 127, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "all?",                  0,   1, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "any?",                  0,   1, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "none?",                 0,   1, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "one?",                  0,   1, BF_ANY,      TY_BOOL,       BOPE_NONE },
  { BOP_ANY_ARRAY, "select!",               0, 127, BF_REQUIRED, TY_POLY,       BOPE_NONE },  /* self, or nil when nothing was removed */
  { BOP_ANY_ARRAY, "filter!",               0, 127, BF_REQUIRED, TY_POLY,       BOPE_NONE },
  { BOP_ANY_ARRAY, "reject!",               0, 127, BF_REQUIRED, TY_POLY,       BOPE_NONE },
  { BOP_ANY_ARRAY, "flatten!",              1,   1, BF_ANY,      TY_POLY,       BOPE_ARRAY_FLATTEN },  /* self or nil */
  { BOP_ANY_ARRAY, "uniq!",                 0,   0, BF_NONE,     TY_POLY,       BOPE_TEMPLATE, "sp_$AArray_uniq_bangq($r)" },  /* self, or nil when a no-op */
  { BOP_ANY_ARRAY, "compact!",              0,   0, BF_NONE,     TY_POLY,       BOPE_ARRAY_COMPACT_BANG },
  { BOP_ANY_ARRAY, "flatten!",              0,   0, BF_NONE,     TY_POLY,       BOPE_ARRAY_COMPACT_BANG },
  { BOP_ANY_ARRAY, "keep_if",               0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },  /* always self */
  { BOP_ANY_ARRAY, "delete_if",             0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "each_index",            0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "reverse",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },  /* flatten: typed arrays have no nesting */
  { BOP_ANY_ARRAY, "sort",                  0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "uniq",                  0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "to_a",                  0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "to_ary",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "deconstruct",           0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "entries",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "dup",                   0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "clone",                 0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "compact",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "flatten",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "clear",                 0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "transpose",             0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "shuffle",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "reverse!",              0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "sort!",                 0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "shuffle!",              0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "rotate!",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "rotate",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "insert",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "concat",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "freeze",                0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "replace",               0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "values_at",             0, 127, BF_ANY,      BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "union",                 0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_union($r, NULL)" },
  { BOP_ANY_ARRAY, "fetch_values",          0, 127, BF_NONE,     BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "fetch_values",          0, 127, BF_REQUIRED, TY_POLY_ARRAY, BOPE_NONE },  /* fallback values mix in (#2368) */
  { BOP_ANY_ARRAY, "zip",                   0, 127, BF_NONE,     TY_POLY_ARRAY, BOPE_NONE },
  { BOP_ANY_ARRAY, "zip",                   0, 127, BF_REQUIRED, TY_NIL,        BOPE_NONE },  /* the block form returns nil */
  { BOP_ANY_ARRAY, "product",               1, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },  /* the block form returns self */
  { BOP_ANY_ARRAY, "product",               1, 127, BF_ANY,      TY_POLY_ARRAY, BOPE_NONE },
  { BOP_ANY_ARRAY, "product",               0,   0, BF_NONE,     TY_POLY_ARRAY, BOPE_ARRAY_PRODUCT },
  { BOP_ANY_ARRAY, "combination",           0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },  /* block forms return self */
  { BOP_ANY_ARRAY, "permutation",           0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "repeated_combination",  0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "repeated_permutation",  0, 127, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "each_slice",            1,   1, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },  /* Ruby >= 3.1: the block form returns self */
  { BOP_ANY_ARRAY, "each_cons",             1,   1, BF_REQUIRED, BOPR_SELF,     BOPE_NONE },
  { BOP_ANY_ARRAY, "delete",                1,   1, BF_REQUIRED, TY_POLY,       BOPE_NONE },  /* the not-found block's value mixes in */
  { BOP_ANY_ARRAY, "delete",                1,   1, BF_ANY,      BOPR_ELEM,     BOPE_NONE },
  { BOP_ANY_ARRAY, "delete_at",             1,   1, BF_ANY,      BOPR_ELEM,     BOPE_TEMPLATE, "sp_$AArray_delete_at($h, $i0)" },

  /* Array, any kind: the codegen rows emit_array_call looks up before its
     typed-array and poly-array arms ($A names the variant). A row narrower
     than the inference row of its name answers what that row answers; one
     with no inference row of its own answers what the rules after the
     lookup answered for any Array receiver. take/drop (a lazy receiver
     node), dig(i) (a splat), slice!(x), sum(seed), +, the set operations
     with operands (the operand's kind), push/<</append (a literal
     receiver), assoc/rassoc (a typed receiver falls to the reopen rules)
     and the block forms of uniq!/compact!/flatten! carry none. */
  { BOP_ANY_ARRAY, "length",                0,   0, BF_NONE,     TY_INT,        BOPE_TEMPLATE, "sp_$AArray_length($r)" },
  { BOP_ANY_ARRAY, "size",                  0,   0, BF_NONE,     TY_INT,        BOPE_TEMPLATE, "sp_$AArray_length($r)" },
  { BOP_ANY_ARRAY, "count",                 0,   0, BF_NONE,     TY_INT,        BOPE_TEMPLATE, "sp_$AArray_length($r)" },
  { BOP_ANY_ARRAY, "empty?",                0,   0, BF_ANY,      TY_BOOL,       BOPE_TEMPLATE, "(sp_$AArray_length($r) == 0)" },
  { BOP_ANY_ARRAY, "first",                 0,   0, BF_ANY,      BOPR_ELEM,     BOPE_TEMPLATE, "sp_$AArray_get($r, 0)" },
  { BOP_ANY_ARRAY, "shift",                 0,   0, BF_ANY,      BOPR_ELEM,     BOPE_TEMPLATE, "sp_$AArray_shift($r)" },  /* the nil sentinel when empty */
  { BOP_ANY_ARRAY, "pop",                   0,   0, BF_ANY,      BOPR_ELEM,     BOPE_TEMPLATE, "sp_$AArray_pop($r)" },
  { BOP_ANY_ARRAY, "inspect",               0,   0, BF_ANY,      TY_STRING,     BOPE_TEMPLATE, "sp_$AArray_inspect($r)" },
  { BOP_ANY_ARRAY, "to_a",                  0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "$r" },
  { BOP_ANY_ARRAY, "to_ary",                0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "$r" },
  { BOP_ANY_ARRAY, "entries",               0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "$r" },
  { BOP_ANY_ARRAY, "deconstruct",           0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "$r" },
  { BOP_ANY_ARRAY, "dup",                   0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_dup($r)" },
  { BOP_ANY_ARRAY, "clone",                 0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; sp_$AArray *_t$u = sp_$AArray_dup(_t$t); _t$u->frozen = _t$t ? _t$t->frozen : 0; _t$u; })" },  /* the frozen flag carries over */
  { BOP_ANY_ARRAY, "compact",               0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_compact($r)" },  /* drops the nil sentinel */
  { BOP_ANY_ARRAY, "shuffle",               0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_shuffle($r)" },
  { BOP_ANY_ARRAY, "reverse",               0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = sp_$AArray_dup($r); sp_$AArray_reverse_bang(_t$t); _t$t; })" },
  { BOP_ANY_ARRAY, "clear",                 0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; if (_t$t && _t$t->frozen) sp_raise_cls(\"FrozenError\", sp_sprintf(\"can't modify frozen Array: %s\", sp_$AArray_inspect(_t$t))); if (_t$t) _t$t->len = 0; _t$t; })" },
  { BOP_ANY_ARRAY, "values_at",             0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "((void)($r), sp_$AArray_new())" },  /* no indices: empty (#2980) */
  { BOP_ANY_ARRAY, "intersection",          0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_dup($r)" },  /* a fold over nothing: a copy (#3851) */
  { BOP_ANY_ARRAY, "difference",            0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "sp_$AArray_dup($r)" },
  { BOP_ANY_ARRAY, "insert",                1,   1, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; SP_GC_ROOT(_t$t); (void)($i0); _t$t; })" },  /* no values: self */
  { BOP_ANY_ARRAY, "take",                  1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u < 0) sp_raise_cls(\"ArgumentError\", \"attempt to take negative size\"); sp_$AArray_slice(_t$t, 0, _t$u); })" },
  { BOP_ANY_ARRAY, "drop",                  1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; SP_GC_ROOT(_t$t); sp_int _t$u = $i0; if (_t$u < 0) sp_raise_cls(\"ArgumentError\", \"attempt to drop negative size\"); sp_$AArray_slice(_t$t, _t$u, _t$t->len - _t$u); })" },
  { BOP_ANY_ARRAY, "last",                  0,   0, BF_ANY,      BOPR_ELEM,     BOPE_ARRAY_LAST },
  { BOP_ANY_ARRAY, "join",                  0,   1, BF_ANY,      TY_STRING,     BOPE_ARRAY_JOIN },
  { BOP_ANY_ARRAY, "[]",                    2,   2, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $gsp_int _t$u = $i0; sp_int _t$v = $i1; sp_int _t$w = sp_$AArray_length(_t$t); (_t$v < 0 || _t$u > _t$w || _t$u < -_t$w) ? (sp_$AArray *)0 : sp_$AArray_slice(_t$t, _t$u, _t$v); })" },
  { BOP_ANY_ARRAY, "dig",                   1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "sp_$AArray_get($h, $i0)" },  /* one step: arr[i] */
  { BOP_ANY_ARRAY, "slice!",                1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SLICE_BANG_RANGE, NULL, BOP_K(TY_RANGE) },
  { BOP_ANY_ARRAY, "slice!",                1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_TEMPLATE, "sp_$AArray_delete_at($h, $i0)" },  /* the element, or nil */
  { BOP_ANY_ARRAY, "uniq!",                 0,   0, BF_REQUIRED, TY_UNKNOWN,    BOPE_TEMPLATE, "sp_$AArray_uniq_bangq($r)" },
  { BOP_ANY_ARRAY, "reverse!",              0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; sp_$AArray_reverse_bang(_t$t); _t$t; })" },
  { BOP_ANY_ARRAY, "shuffle!",              0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $r; sp_$AArray_shuffle_bang(_t$t); _t$t; })" },
  { BOP_ANY_ARRAY, "sort!",                 0,   0, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_SORT_BANG },
  { BOP_ANY_ARRAY, "rotate!",               0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $gsp_$AArray_rotate_bang(_t$t, 1); _t$t; })" },
  { BOP_ANY_ARRAY, "rotate!",               1,   1, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = $gsp_$AArray_rotate_bang(_t$t, $i0); _t$t; })" },
  { BOP_ANY_ARRAY, "rotate",                0,   0, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = sp_$AArray_dup($r); SP_GC_ROOT(_t$t); sp_$AArray_rotate_bang(_t$t, 1); _t$t; })" },
  { BOP_ANY_ARRAY, "rotate",                1,   1, BF_ANY,      BOPR_SELF,     BOPE_TEMPLATE, "({ sp_$AArray *_t$t = sp_$AArray_dup($r); SP_GC_ROOT(_t$t); sp_$AArray_rotate_bang(_t$t, $i0); _t$t; })" },
  { BOP_ANY_ARRAY, "minmax",                0,   0, BF_NONE,     BOPR_SELF,     BOPE_ARRAY_MINMAX },
  { BOP_ANY_ARRAY, "sort",                  0,   0, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_SORT },
  { BOP_ANY_ARRAY, "uniq",                  0,   0, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_UNIQ },
  { BOP_ANY_ARRAY, "replace",               1,   1, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_REPLACE },
  { BOP_ANY_ARRAY, "+",                     1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_PLUS },
  { BOP_ANY_ARRAY, "&",                     1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "|",                     1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "-",                     1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "intersection",          1, 127, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "union",                 1, 127, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "difference",            1, 127, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_SETOP },
  { BOP_ANY_ARRAY, "intersect?",            1,   1, BF_ANY,      TY_BOOL,       BOPE_ARRAY_INTERSECT_P },
  { BOP_ANY_ARRAY, "sum",                   0,   0, BF_NONE,     BOPR_ARRAY_SUM, BOPE_ARRAY_SUM0 },
  { BOP_ANY_ARRAY, "compact!",              0,   0, BF_REQUIRED, TY_UNKNOWN,    BOPE_ARRAY_COMPACT_BANG },
  { BOP_ANY_ARRAY, "flatten!",              0,   0, BF_REQUIRED, TY_UNKNOWN,    BOPE_ARRAY_COMPACT_BANG },
  { BOP_ANY_ARRAY, "flatten",               0,   1, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_FLATTEN },
  { BOP_ANY_ARRAY, "push",                  1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_PUSH },
  { BOP_ANY_ARRAY, "<<",                    1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_PUSH },
  { BOP_ANY_ARRAY, "append",                1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_PUSH },
  { BOP_ANY_ARRAY, "insert",                2, 127, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_INSERT_N },
  { BOP_ANY_ARRAY, "transpose",             0,   0, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_TRANSPOSE },
  { BOP_ANY_ARRAY, "assoc",                 1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_ASSOC },
  { BOP_ANY_ARRAY, "rassoc",                1,   1, BF_ANY,      TY_UNKNOWN,    BOPE_ARRAY_ASSOC },
  { BOP_ANY_ARRAY, "combination",           1,   1, BF_NONE,     BOPR_ARRAY_TUPLES, BOPE_ARRAY_COMBINATION },
  { BOP_ANY_ARRAY, "repeated_combination",  1,   1, BF_NONE,     BOPR_ARRAY_TUPLES, BOPE_ARRAY_COMBINATION },
  { BOP_ANY_ARRAY, "repeated_permutation",  1,   1, BF_NONE,     BOPR_ARRAY_TUPLES, BOPE_ARRAY_COMBINATION },
  { BOP_ANY_ARRAY, "permutation",           0,   1, BF_NONE,     BOPR_ARRAY_TUPLES, BOPE_ARRAY_COMBINATION },
  { BOP_ANY_ARRAY, "product",               1,   1, BF_NONE,     TY_POLY_ARRAY, BOPE_ARRAY_PRODUCT },
  { BOP_ANY_ARRAY, "fetch_values",          0,   0, BF_NONE,     BOPR_SELF,     BOPE_ARRAY_FETCH_VALUES0 },
  { BOP_ANY_ARRAY, "fetch_values",          0,   0, BF_REQUIRED, TY_POLY_ARRAY, BOPE_ARRAY_FETCH_VALUES0 },
  { BOP_ANY_ARRAY, "all?",                  0,   0, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED0 },
  { BOP_ANY_ARRAY, "any?",                  0,   0, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED0 },
  { BOP_ANY_ARRAY, "none?",                 0,   0, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED0 },
  { BOP_ANY_ARRAY, "one?",                  0,   0, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED0 },
  { BOP_ANY_ARRAY, "dig",                   2, 127, BF_ANY,      TY_POLY,       BOPE_ARRAY_DIG_N },
  { BOP_ANY_ARRAY, "sum",                   1,   1, BF_NONE,     TY_UNKNOWN,    BOPE_ARRAY_SUM1 },
  { BOP_ANY_ARRAY, "concat",                1, 127, BF_ANY,      BOPR_SELF,     BOPE_ARRAY_CONCAT },
  { BOP_ANY_ARRAY, "index",                 1,   1, BF_NONE,     BOPR_ARRAY_INDEX, BOPE_ARRAY_INDEX_V },
  { BOP_ANY_ARRAY, "find_index",            1,   1, BF_NONE,     BOPR_ARRAY_INDEX, BOPE_ARRAY_INDEX_V },
  { BOP_ANY_ARRAY, "rindex",                1,   1, BF_NONE,     BOPR_ARRAY_INDEX, BOPE_ARRAY_INDEX_V },

  /* Array, any kind: emit_call_body's Array arms that run long before
     emit_array_call, each a row of its own stage looked up where its arm sat
     (codegen_call_array.c). cycle with no count answers no kind: the rule
     after the lookup reads whether the call is a chain's receiver. The
     guarded rows are not read by inference, which answers *(String) and the
     predicates with a Class from its own rules. */
  { BOP_ANY_ARRAY, "cycle",                 0,   0, BF_NONE,     TY_UNKNOWN,    BOPE_ARRAY_CYCLE_ENDLESS, 0, 0, 0, 0, 3 },  /* stage 3: emit_blockless_enumerator */
  { BOP_ANY_ARRAY, "*",                     1,   1, BF_ANY,      TY_STRING,     BOPE_ARRAY_JOIN_STR, 0, BOP_K(TY_STRING), 0, 0, 4 },  /* stage 4: a join */
  { BOP_ANY_ARRAY, "any?",                  1,   1, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED_CLASS, 0, BOP_K(TY_CLASS), 0, 0, 5 },  /* stage 5: Class === element */
  { BOP_ANY_ARRAY, "all?",                  1,   1, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED_CLASS, 0, BOP_K(TY_CLASS), 0, 0, 5 },
  { BOP_ANY_ARRAY, "none?",                 1,   1, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED_CLASS, 0, BOP_K(TY_CLASS), 0, 0, 5 },
  { BOP_ANY_ARRAY, "one?",                  1,   1, BF_NONE,     TY_BOOL,       BOPE_ARRAY_PRED_CLASS, 0, BOP_K(TY_CLASS), 0, 0, 5 },
  /* The reflection lookup alone asks this family, after earlier overrides.
     A set's result depends on its value argument, so inference supplies it. */
  { BOP_IVAR_LESS, "instance_variable_get",      1, 1, BF_NONE, TY_NIL,        BOPE_IVAR_REFLECTION, "get" },
  { BOP_IVAR_LESS, "instance_variable_defined?", 1, 1, BF_NONE, TY_BOOL,       BOPE_IVAR_REFLECTION, "defined" },
  { BOP_IVAR_LESS, "instance_variables",         0, 0, BF_NONE, TY_POLY_ARRAY, BOPE_IVAR_REFLECTION, "list" },
  { BOP_IVAR_LESS, "instance_variable_set",      2, 2, BF_NONE, TY_UNKNOWN,    BOPE_IVAR_REFLECTION, "set" },

};
#define BOP_NROWS ((int)(sizeof bop_rows / sizeof bop_rows[0]))

/* Row indices sorted by (receiver kind, name), built on first use. */
static int bop_index[BOP_NROWS];
static int bop_indexed;

static int bop_cmp_key(TyKind rt, const char *name, const BuiltinOp *r) {
#ifdef SP_WORK_COUNT
  g_nt_work++;   /* a probe is a name compare, as sp_streq counts one */
#endif
  if (rt != r->recv) return rt < r->recv ? -1 : 1;
  return strcmp(name, r->name);
}

/* within a name: narrowest arity first, then by argc_min, then as written */
static int bop_sort_cmp(const void *a, const void *b) {
  int i = *(const int *)a, j = *(const int *)b;
  const BuiltinOp *x = &bop_rows[i], *y = &bop_rows[j];
  int r = bop_cmp_key(x->recv, x->name, y);
  if (r) return r;
  int wx = x->argc_max - x->argc_min, wy = y->argc_max - y->argc_min;
  if (wx != wy) return wx - wy;
  if (x->argc_min != y->argc_min) return x->argc_min - y->argc_min;
  return i - j;
}

#ifndef NDEBUG
/* Two emitting rows of one name in different stages must never both fit a
   call: each would emit it at its own place in the chain, and which one ran
   would depend on which lookup the call reached first. They are kept apart
   by arity, by block form, or by an argument guard. */
static int bop_kinds_meet(BopKinds a, BopKinds b) { return !a || !b || (a & b); }
static void bop_check_stages(void) {
  for (int i = 0; i < BOP_NROWS; i++) {
    const BuiltinOp *x = &bop_rows[bop_index[i]];
    for (int j = i + 1; j < BOP_NROWS; j++) {
      const BuiltinOp *y = &bop_rows[bop_index[j]];
      if (bop_cmp_key(x->recv, x->name, y) != 0) break;
      if (x->stage == y->stage || x->emit == BOPE_NONE || y->emit == BOPE_NONE) continue;
      int argc_meet = x->argc_min <= y->argc_max && y->argc_min <= x->argc_max;
      int block_meet = !((x->block == BF_NONE && y->block == BF_REQUIRED) ||
                         (x->block == BF_REQUIRED && y->block == BF_NONE));
      assert(!(argc_meet && block_meet && bop_kinds_meet(x->arg0, y->arg0) &&
               bop_kinds_meet(x->arg1, y->arg1)));
      (void)argc_meet; (void)block_meet;
    }
  }
}
#endif

static void bop_build_index(void) {
  for (int i = 0; i < BOP_NROWS; i++) bop_index[i] = i;
  qsort(bop_index, BOP_NROWS, sizeof bop_index[0], bop_sort_cmp);
#ifndef NDEBUG
  bop_check_stages();
#endif
  bop_indexed = 1;
}

static int bop_row_fits(const BuiltinOp *r, int argc, int has_block) {
  if (argc < r->argc_min || argc > r->argc_max) return 0;
  if (r->block == BF_NONE && has_block) return 0;
  if (r->block == BF_REQUIRED && !has_block) return 0;
  return 1;
}

int bop_row_count(void) { return (int)(sizeof bop_rows / sizeof bop_rows[0]); }
const BuiltinOp *bop_row(int i) { return i >= 0 && i < bop_row_count() ? &bop_rows[i] : NULL; }

const BuiltinOp *bop_find(TyKind rt, const char *name, int argc, int has_block) {
  return bop_find_arg(rt, name, argc, has_block, NULL, NULL);
}

const BuiltinOp *bop_find_arg(TyKind rt, const char *name, int argc, int has_block,
                              BopArgKind arg_of, const void *ud) {
  return bop_find_stage(rt, name, argc, has_block, arg_of, ud, -1);
}
const BuiltinOp *bop_find_stage(TyKind rt, const char *name, int argc, int has_block,
                                BopArgKind arg_of, const void *ud, int stage) {
  if (!name) return NULL;
  if (!bop_indexed) bop_build_index();
  int lo = 0, hi = BOP_NROWS;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (bop_cmp_key(rt, name, &bop_rows[bop_index[mid]]) > 0) lo = mid + 1;
    else hi = mid;
  }
  /* rows of one name differ by arity, block form or argument guard: take
     the first that fits */
  TyKind ak[2] = { TY_UNKNOWN, TY_UNKNOWN };
  int ak_known[2] = { 0, 0 };
  for (int i = lo; i < BOP_NROWS; i++) {
    const BuiltinOp *r = &bop_rows[bop_index[i]];
    if (bop_cmp_key(rt, name, r) != 0) break;
    if (stage >= 0 && r->stage != stage) continue;
    if (!bop_row_fits(r, argc, has_block)) continue;
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
      BopKinds want = k == 0 ? r->arg0 : r->arg1;
      if (!want) continue;
      if (!arg_of || argc <= k) { ok = 0; break; }
      if (!ak_known[k]) { ak[k] = arg_of(ud, k); ak_known[k] = 1; }
      /* an object kind has no bit (TY_OBJECT_BASE and up): no guard holds it */
      if ((unsigned)ak[k] >= 64 || !(want & BOP_K(ak[k]))) ok = 0;
    }
    if (ok) return r;
  }
  return NULL;
}

const BuiltinOp *bop_find_boxed(TyKind rt, const char *name, int argc, int has_block) {
  const BuiltinOp *op = bop_find(rt, name, argc, has_block);
  return op && (op->flags & BOPF_BOXED) ? op : NULL;
}

TyKind bop_result(const BuiltinOp *op, TyKind rt) {
  switch ((int)op->result) {
  case (int)BOPR_SELF:      return rt;
  case (int)BOPR_HASH_VAL:  return ty_hash_val(rt);
  case (int)BOPR_HASH_KEYS: return ty_array_of(ty_hash_key(rt));
  case (int)BOPR_HASH_VALS: return ty_array_of(ty_hash_val(rt));
  case (int)BOPR_ELEM:      return ty_array_elem(rt);
  case (int)BOPR_HASH_KEY_OF: return rt == TY_SYM_POLY_HASH ? TY_SYMBOL : TY_POLY;
  case (int)BOPR_HASH_INVERT: return rt == TY_STR_STR_HASH ? TY_STR_STR_HASH : TY_POLY_POLY_HASH;
  case (int)BOPR_ARRAY_SUM: return rt == TY_STR_ARRAY ? TY_POLY : ty_array_elem(rt);
  case (int)BOPR_ARRAY_INDEX:
    return rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY ? TY_POLY : TY_INT;
  case (int)BOPR_ARRAY_TUPLES: return rt == TY_POLY_ARRAY ? TY_POLY_ARRAY : TY_ENUMERATOR;
  default:                  return op->result;
  }
}

int bop_covers(TyKind rt) {
  if (!bop_indexed) bop_build_index();
  int lo = 0, hi = BOP_NROWS;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (bop_rows[bop_index[mid]].recv < rt) lo = mid + 1;
    else hi = mid;
  }
  return lo < BOP_NROWS && bop_rows[bop_index[lo]].recv == rt;
}
