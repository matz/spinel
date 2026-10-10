#ifndef SP_RE_H
#define SP_RE_H
/* sp_re.h -- regexp wrappers + MatchData, compiled once in lib/sp_re.c.
 *
 * The regexp ENGINE (re_compile / re_exec / re_free) is the separate
 * build/regexp/*.o library; this layer is spinel's Ruby-facing surface
 * (Regexp#match, gsub/sub/scan/split, the $~/$1.. match registers, and
 * MatchData). The match-register globals are per-process singletons: the
 * generated TU reads them directly for $&/$1.. (codegen_expr.c) and marks
 * them in its whole-program GC root hook, so they are extern here and
 * defined once in lib/sp_re.c.
 *
 * Kept in spinel_rt.h (TU-coupled): the whole-program GC root hook
 * sp_re_mark_globals, the startup-error handler (longjmps through a
 * TU-local jmp_buf), and the hash-replacement gsub/sub variants (await
 * the typed-hash batch). */
#include "sp_array.h"   /* sp_StrArray / sp_PolyArray / sp_RbVal + heap */
#include "sp_str.h"     /* string helpers used by the wrappers */

/* The engine's internal flag bits. re_internal.h is the port island's own
   header and is not included here, so the three this side reads are named
   again; sp_re_raw_flags() answers a pattern's set. */
#define SP_RE_F_IGNORECASE 1
#define SP_RE_F_DOTALL     4   /* Ruby's /m: dot matches newline */
#define SP_RE_F_EXTENDED   8

/* ---- regexp engine ABI (implemented in build/regexp/*.o) ---- */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
mrb_regexp_pattern* re_compile(const char *pattern, int64_t len, uint32_t flags);
void re_free(mrb_regexp_pattern *pat);
int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size, int binary);
/* named-capture introspection (lib/regexp/re_compile.c) */
int re_num_named(const mrb_regexp_pattern *pat);
const char *re_named_name(const mrb_regexp_pattern *pat, int i, int *group_out);
int re_named_group(const mrb_regexp_pattern *pat, const char *name);

/* `caps` holds ncap*2 positions and lives in the SAME block as the struct, so
   a MatchData costs what its match needs rather than what the widest one
   would: it was a fixed caps[64], which is 256 bytes whether the pattern has
   thirty groups or none, and every MatchData paid it. A 1-group match is 40
   bytes now against 280. Nothing takes sizeof() of this outside sp_md_alloc(),
   which is what a flexible array member costs. */
typedef struct { const char *source; int ncap; const mrb_regexp_pattern *pat; int caps[]; } sp_MatchData;

/* ---- match-register state (read by the generated TU and marked by its GC root
   hook). Per-thread in Ruby, so SP_TLS in the threaded build: each worker keeps
   the registers of the green thread it runs. The generated TU is compiled with
   the matching -DSP_THREADS so the storage class agrees. Plain globals
   otherwise. (The GC hook marking only the current worker's registers is
   generalized to all workers when N>1 lands.) ---- */
extern SP_TLS const char *sp_re_captures[10];
extern SP_TLS int sp_re_caps[64];
extern SP_TLS const char *sp_re_last_str;
extern SP_TLS const mrb_regexp_pattern *sp_re_last_pat;
extern SP_TLS const char *sp_re_match_str;
extern SP_TLS const char *sp_re_match_pre;
extern SP_TLS const char *sp_re_match_post;
/* Whether the last sub / gsub (regex or string pattern) found a match: the
   bang forms answer nil when NO SUBSTITUTION was made, which is not the same
   as the text being unchanged (`"cats".sub!(/s$/, "s")` matches and answers
   the string). The emitter clears it before the call and reads it after. */
extern SP_TLS int sp_re_sub_matched;
const char *sp_re_pre_match(void);   /* $` , built on demand */
const char *sp_re_post_match(void);  /* $' , built on demand */
extern const char *sp_re_startup_err;

/* Stop-the-world support: push this worker's live match-register strings as GC
   roots (for the collector to mark while the worker is parked). See sp_re.c. */
void sp_re_push_match_roots(void);

/* ---- wrappers (lib/sp_re.c) ---- */
const char *sp_re_last_paren_match(void);
void sp_re_set_captures(const char *str, int *caps, int ncaps);
extern int sp_re_track_last;   /* gsub / sub / scan record `$~` (sp_re.c) */
void sp_re_clear_last_match(void);
void sp_re_set_last_match(const mrb_regexp_pattern *pat, const char *str, const int *caps, int n);
void sp_re_set_lit_match(const char *str, sp_int beg, sp_int end);
sp_int sp_re_match(mrb_regexp_pattern *pat, const char *str);
sp_int sp_re_match_at(mrb_regexp_pattern *pat, const char *str, sp_int pos);
sp_int sp_re_match_next(mrb_regexp_pattern *pat, const char *str, sp_int pos);
sp_int sp_re_rindex(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_rpartition(mrb_regexp_pattern *pat, const char *str);
sp_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str);
sp_bool sp_re_match_p_at(mrb_regexp_pattern *pat, const char *str, sp_int pos);
sp_bool sp_re_case_eq(mrb_regexp_pattern *pat, sp_RbVal v);
/* poly-operand match forms: either side may carry the pattern (#3961) */
mrb_regexp_pattern *sp_poly_as_pattern(sp_RbVal v);
sp_bool sp_poly_match_p(sp_RbVal a, sp_RbVal b);
sp_bool sp_poly_match_p_at(sp_RbVal a, sp_RbVal b, sp_int pos);
sp_MatchData *sp_poly_match_data(sp_RbVal a, sp_RbVal b);
sp_oint sp_poly_match_index(sp_RbVal a, sp_RbVal b);   /* =~: the position, nil on a miss */
void sp_re_expand_rep(const mrb_regexp_pattern *pat, char **out_io, size_t *olen_io, size_t *cap_io, const char *rep, size_t rlen, const char *src, int *caps, int ncaps);
const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep);
const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep);
sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_split_limit(mrb_regexp_pattern *pat, const char *str, sp_int limit);
sp_oint sp_re_rindex_opt(mrb_regexp_pattern *pat, const char *str);
sp_RbVal sp_re_rindex_poly(mrb_regexp_pattern *pat, const char *str);
sp_RbVal sp_re_index_poly(mrb_regexp_pattern *pat, const char *str);
const char *sp_str_splice_re(mrb_regexp_pattern *pat, const char *s, const char *val);
const char *sp_str_slice_re(mrb_regexp_pattern *pat, const char *s, const char **rest_out);
const char *sp_re_alt_join(const char *a, const char *b);  /* `a|b`, NUL-safe */
const char *sp_re_source(void *pat);
uint32_t sp_re_source_len(void *pat);   /* its byte length (may hold a NUL) */
sp_bool sp_re_src_linear_time(const char *src);   /* Regexp.linear_time? on a source string */
const char *sp_re_inspect_str(void *pat);
const char *sp_re_to_s_str(void *pat);
sp_int sp_re_options(void *pat);
sp_int sp_re_hash(void *pat);
sp_bool sp_re_eq(void *a, void *b);
sp_bool sp_re_casefold_p(void *pat);
uint32_t sp_re_raw_flags(void *pat);
uint32_t sp_re_opts_to_flags(sp_int o);
const char *sp_MatchData_inspect(sp_MatchData *m);
const char *re_group_name(const mrb_regexp_pattern *pat, int group);
/* String#index / #rindex / #byteindex / #byterindex with a Regexp: the
   position, nil on a miss */
sp_oint sp_re_index_from_opt(mrb_regexp_pattern *pat, const char *str, sp_int start);
sp_oint sp_re_byteindex_opt(mrb_regexp_pattern *pat, const char *str, sp_int start);
sp_oint sp_re_byterindex_opt(mrb_regexp_pattern *pat, const char *str, sp_int start);
sp_oint sp_re_rindex_from_opt(mrb_regexp_pattern *pat, const char *str, sp_int start);
sp_RbVal sp_re_match_poly(mrb_regexp_pattern *pat, const char *str);
const char *sp_re_named_capture(const mrb_regexp_pattern *pat, const char *name);
const char *sp_re_escape(const char *src);
mrb_regexp_pattern *sp_re_union_array(sp_PolyArray *a);
sp_PolyArray *sp_re_scan_poly(mrb_regexp_pattern *pat, const char *str);
sp_PolyArray *sp_re_match_data(mrb_regexp_pattern *pat, const char *str);
void sp_MatchData_scan(void *p);
sp_MatchData *sp_re_matchdata(mrb_regexp_pattern *pat, const char *str);
sp_MatchData *sp_re_last_matchdata(void);   /* $~ from the TLS match registers */
/* Saved match registers of one method frame: `$~` is frame-local in Ruby, so a
   method that performs a match restores its caller's on the way out (#3629).
   The emitter declares one with a cleanup attribute, so every ordinary exit
   path -- including an early `return` -- puts the caller's registers back. */
typedef struct {
  const char *captures[10];
  int caps[64];
  const char *last_str, *match_str, *match_pre, *match_post;
  int last_ncap;
  const mrb_regexp_pattern *last_pat;
  int last_lit;
  int pp_span[2];
  int nroots;   /* sp_gc_nroots before sp_re_frame_push rooted the saved strings */
} sp_re_frame;
void sp_re_frame_push(sp_re_frame *f);
void sp_re_frame_pop(sp_re_frame *f);
sp_MatchData *sp_re_matchdata_at(mrb_regexp_pattern *pat, const char *str, sp_int cpos);
const char *sp_MatchData_aref(sp_MatchData *m, sp_int i);
const char *sp_MatchData_aref_name(sp_MatchData *m, const char *name);
sp_StrArray *sp_MatchData_names(sp_MatchData *m);
const char *sp_MatchData_string(sp_MatchData *m);
sp_StrArray *sp_Regexp_names(const mrb_regexp_pattern *pat);
/* named-group metadata (lib/regexp/re_compile.c), for the generated TU's
   inline Regexp#named_captures construction */
int re_num_named(const mrb_regexp_pattern *pat);
const char *re_named_name(const mrb_regexp_pattern *pat, int i, int *group_out);
sp_int sp_MatchData_length(sp_MatchData *m);
sp_bool sp_MatchData_eq(sp_MatchData *a, sp_MatchData *b);
sp_int sp_MatchData_hash(sp_MatchData *m);
sp_PolyArray *sp_MatchData_aref_len(sp_MatchData *m, sp_int start, sp_int len);
sp_PolyArray *sp_MatchData_aref_range(sp_MatchData *m, sp_int beg, sp_int end, int excl);
sp_oint sp_md_char_off(sp_MatchData *m, int byteoff);   /* nil for an unmatched group (byteoff < 0) */
/* MatchData#begin / #end / #bytebegin / #byteend: nil for a group that did not take part */
sp_oint sp_MatchData_begin(sp_MatchData *m, sp_int i);
sp_oint sp_MatchData_end(sp_MatchData *m, sp_int i);
sp_IntArray *sp_MatchData_offset(sp_MatchData *m, sp_int i);
sp_oint sp_MatchData_bytebegin(sp_MatchData *m, sp_int i);
sp_oint sp_MatchData_byteend(sp_MatchData *m, sp_int i);
sp_IntArray *sp_MatchData_byteoffset(sp_MatchData *m, sp_int i);
sp_oint sp_MatchData_begin_name(sp_MatchData *m, const char *name);
sp_oint sp_MatchData_end_name(sp_MatchData *m, const char *name);
sp_IntArray *sp_MatchData_offset_name(sp_MatchData *m, const char *name);
sp_oint sp_MatchData_bytebegin_name(sp_MatchData *m, const char *name);
sp_oint sp_MatchData_byteend_name(sp_MatchData *m, const char *name);
sp_IntArray *sp_MatchData_byteoffset_name(sp_MatchData *m, const char *name);
const char *sp_MatchData_to_s(sp_MatchData *m);
sp_PolyArray *sp_MatchData_captures(sp_MatchData *m);
sp_PolyArray *sp_MatchData_to_a(sp_MatchData *m);
const char *sp_MatchData_pre_match(sp_MatchData *m);
const char *sp_MatchData_post_match(sp_MatchData *m);
void sp_re_default_error_handler(const char *msg);

/* ---- sp_str_re_match_p_at relocated from spinel_rt.h (0 optcarrot uses).
   Lives here (not sp_str.h) because spinel_rt.h includes sp_re.h before
   sp_str.h -- placing it in sp_str.h would see mrb_regexp_pattern
   undeclared on that first (nested) pass. ---- */
sp_bool sp_str_re_match_p_at(mrb_regexp_pattern *pat, const char *str, sp_int cpos);

#endif /* SP_RE_H */
