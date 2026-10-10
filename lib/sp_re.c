/* sp_re.c -- regexp wrappers + MatchData (see sp_re.h).
 *
 * Moved out of spinel_rt.h so this layer compiles once into
 * libspinel_rt.a. It calls the regexp engine (re_*) and the shared string
 * heap / arrays; sp_sprintf / sp_raise_cls resolve at the final link. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sp_re.h"
#include "sp_string.h"   /* sp_String builder */
#include "sp_inspect.h"  /* sp_inspect_container (poly element render) */
#include "sp_gc.h"       /* sp_sym_name_fn (Symbol operand of === ) */

#ifndef SPL
#define SPL(s) (&("\xff" s)[1])
#endif
const char *sp_sprintf(const char *fmt, ...);  /* defined in the generated TU */

/* match-register state (declared extern in sp_re.h). */
SP_TLS const char *sp_re_captures[10] = {0};   /* per-worker (SP_TLS); see sp_re.h */
SP_TLS int sp_re_caps[64];
SP_TLS int sp_re_sub_matched = 0;
SP_TLS const char *sp_re_last_str = NULL;
SP_TLS const char *sp_re_match_str = NULL;
/* $` and $' are built only when read: a gsub over a large subject matches
   thousands of times, and copying the whole prefix and suffix at every match
   made the scan quadratic. sp_re_pp_span holds the last match's span in
   sp_re_last_str so the accessors below can build them on demand. */
SP_TLS const char *sp_re_match_pre = NULL;
SP_TLS const char *sp_re_match_post = NULL;
static SP_TLS int sp_re_pp_span[2] = {-1, -1};
const char *sp_re_startup_err = NULL;

/* Stop-the-world support: push this worker's live match-register strings onto its
   GC root stack so a collector marks them while the worker is parked at a
   safepoint (the registers are per-worker TLS, so the collector's own globals
   hook does not reach another worker's copy). Only heap strings still unmarked
   (leading byte 0xfe, or 0xfa frozen) are pushed: frozen literals (0xf1) are
   immortal and never swept, and sp_gc_mark only understands those markers. The
   caller snapshots the root stack right after and then restores its depth, so
   these entries live only inside the published snapshot. */
void sp_re_push_match_roots(void) {
#define SP_RE_PUSH_LIVE(addr) do { const char *_s = *(addr); \
    if (_s && ((unsigned char)_s[-1] | 0x04) == 0xfe) _sp_gc_root_push((void **)(addr)); } while (0)
  SP_RE_PUSH_LIVE(&sp_re_last_str);
  for (int i = 0; i < 10; i++) SP_RE_PUSH_LIVE(&sp_re_captures[i]);
  SP_RE_PUSH_LIVE(&sp_re_match_str);
  SP_RE_PUSH_LIVE(&sp_re_match_pre);
  SP_RE_PUSH_LIVE(&sp_re_match_post);
#undef SP_RE_PUSH_LIVE
}

const char *sp_re_last_paren_match(void) {
  for (int i = 9; i >= 1; i--) {
    if (sp_re_captures[i]) return sp_re_captures[i];
  }
  return NULL;
}
void sp_MatchData_scan(void *p);   /* defined below */
static sp_MatchData *sp_md_alloc(int pairs);   /* defined below */
SP_TLS int sp_re_last_ncap = 0;
SP_TLS const mrb_regexp_pattern *sp_re_last_pat = NULL;
/* Set when the last match was a String pattern's (gsub / sub / scan with a
   String). CRuby's `$~.regexp` for one is the String escaped into a Regexp,
   which the String scan itself never needs, so it is built only when `$~` is
   read (sp_re_lit_pattern). The matched text is the pattern's own bytes, so
   sp_re_match_str is what it is built from. */
SP_TLS int sp_re_last_lit = 0;
/* The Regexp a String-pattern match answers for `$~.regexp`. The last one
   built is kept, so reading `$~` after every turn of a loop that does the same
   `gsub("x", ...)` compiles once. A pattern it replaces is not freed: a
   MatchData or a saved frame may still hold it. */
static const mrb_regexp_pattern *sp_re_lit_pattern(const char *lit) {
  static SP_TLS char *src = NULL;
  static SP_TLS size_t srclen = 0;
  static SP_TLS mrb_regexp_pattern *pat = NULL;
  size_t n = sp_str_byte_len(lit);
  if (pat && n == srclen && memcmp(src, lit, n) == 0) return pat;
  const char *esc = sp_re_escape(lit);
  mrb_regexp_pattern *p = re_compile(esc, (int64_t)sp_str_byte_len(esc), 0);
  if (!p) return NULL;
  char *copy = (char *)malloc(n + 1);
  memcpy(copy, lit, n); copy[n] = 0;
  free(src);
  src = copy; srclen = n; pat = p;
  return p;
}
/* $~ as a first-class MatchData: build it lazily from the TLS match
   registers (NULL when the last match failed / none ran). */
sp_MatchData *sp_re_last_matchdata(void) {
  if (!sp_re_last_str || sp_re_last_ncap <= 0 || sp_re_caps[0] < 0) return NULL;
  if (sp_re_last_lit && sp_re_match_str) {
    sp_re_last_pat = sp_re_lit_pattern(sp_re_match_str);
    sp_re_last_lit = 0;
  }
  int n = sp_re_last_ncap * 2;
  if (n > 64) n = 64;
  sp_MatchData *md = sp_md_alloc(n / 2);
  md->source = sp_re_last_str;
  for (int i = 0; i < n; i++) md->caps[i] = sp_re_caps[i];
  md->ncap = sp_re_last_ncap;
  md->pat = sp_re_last_pat;
  return md;
}
/* CRuby's $~ (and the $1..$9 / $` / $' derived from it) is a FRAME-local, so
   a match inside a method leaves the caller's registers alone once the method
   returns. The registers here are per-worker globals, so a method that matches
   saves them on entry and puts them back on the way out; the emitter gives
   such a method one of these frames (#3629). */
void sp_re_frame_push(sp_re_frame *f) {
  if (!f) return;
  for (int i = 0; i < 10; i++) f->captures[i] = sp_re_captures[i];
  for (int i = 0; i < 64; i++) f->caps[i] = sp_re_caps[i];
  f->last_str = sp_re_last_str;
  f->match_str = sp_re_match_str;
  f->match_pre = sp_re_match_pre;
  f->match_post = sp_re_match_post;
  f->last_ncap = sp_re_last_ncap;
  f->last_pat = sp_re_last_pat;
  f->last_lit = sp_re_last_lit;
  f->pp_span[0] = sp_re_pp_span[0]; f->pp_span[1] = sp_re_pp_span[1];
  /* The saved strings are the only reference to the caller's matches once the
     callee's own match overwrites the registers, and this struct is on the C
     stack where the collector does not look. Root its string slots on the GC
     root stack (tag 2): a rescue landing resets that stack to its try-entry
     depth, and a fiber switch swaps it, so the entries follow the frame. */
  f->nroots = sp_gc_nroots;
  for (int i = 0; i < 10; i++) _sp_gc_root_push((void **)((uintptr_t)&f->captures[i] | (uintptr_t)2));
  _sp_gc_root_push((void **)((uintptr_t)&f->last_str | (uintptr_t)2));
  _sp_gc_root_push((void **)((uintptr_t)&f->match_str | (uintptr_t)2));
  _sp_gc_root_push((void **)((uintptr_t)&f->match_pre | (uintptr_t)2));
  _sp_gc_root_push((void **)((uintptr_t)&f->match_post | (uintptr_t)2));
}
void sp_re_frame_pop(sp_re_frame *f) {
  if (!f) return;
  for (int i = 0; i < 10; i++) sp_re_captures[i] = f->captures[i];
  for (int i = 0; i < 64; i++) sp_re_caps[i] = f->caps[i];
  sp_re_last_str = f->last_str;
  sp_re_match_str = f->match_str;
  sp_re_match_pre = f->match_pre;
  sp_re_match_post = f->match_post;
  sp_re_last_ncap = f->last_ncap;
  sp_re_last_pat = f->last_pat;
  sp_re_last_lit = f->last_lit;
  /* the span $` and $' are built from lazily: the caller's, not the callee's */
  sp_re_pp_span[0] = f->pp_span[0]; sp_re_pp_span[1] = f->pp_span[1];
  /* drop the slots sp_re_frame_push rooted */
  if (sp_gc_nroots > f->nroots) sp_gc_nroots = f->nroots;
}
void sp_re_set_captures(const char *str, int *caps, int ncaps) {SP_GC_ROOT_STR(str);
  sp_re_last_str = str;
  sp_re_last_ncap = ncaps;
  sp_re_last_lit = 0;
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  for (int i = 1; i < ncaps && i < 10; i++) {
    if (caps[i*2] >= 0 && caps[(i*2)+1] >= 0) {
      int len = caps[(i*2)+1] - caps[i*2];
      char *buf = sp_str_alloc_raw(len+1);
      memcpy(buf, str+caps[i*2], len); buf[len] = 0;
      sp_str_set_len(buf, (size_t)len);
      sp_re_captures[i] = buf;
    }
  }
  /* Populate the symbolic back-references from caps[0]/[1] (the whole
     match span). NULL when the match failed; the codegen ternary
     falls back to "". */
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  sp_re_pp_span[0] = sp_re_pp_span[1] = -1;
  if (ncaps >= 1 && caps[0] >= 0 && caps[1] >= 0) {
    int mlen = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(mlen + 1);
    memcpy(m, str + caps[0], mlen); m[mlen] = 0;
    sp_str_set_len(m, (size_t)mlen);
    sp_str_set_len(m, (size_t)mlen);
    sp_re_match_str = m;
    sp_re_pp_span[0] = caps[0]; sp_re_pp_span[1] = caps[1];
  }
}

/* No match: `$~`, `$1`.. and the rest read nil (#848). */
void sp_re_clear_last_match(void) {
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  sp_re_last_str = NULL;
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  sp_re_pp_span[0] = sp_re_pp_span[1] = -1;
}
/* Whether gsub, sub and scan record their last match for `$~`. The
   generated main sets it when the program reads `$~` or what derives from it
   anywhere (g_reads_match_regs); recording copies the match and its groups
   per call, which a program that never reads them should not pay for, so
   the runtime below checks it at each call site. One value for the program,
   set before anything runs, so a plain global. */
int sp_re_track_last = 0;
/* gsub, sub and scan leave `$~` at their LAST match, or nil when nothing
   matched, as CRuby's do. They scan with a caps array of their own, so they
   hand the last match's `n` positions to the registers once they are done. */
void sp_re_set_last_match(const mrb_regexp_pattern *pat, const char *str, const int *caps, int n) {
  if (n <= 0) { sp_re_clear_last_match(); return; }
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) sp_re_caps[i] = caps[i];
  sp_re_last_pat = pat;
  sp_re_set_captures(str, sp_re_caps, n / 2);
}
/* The same for a String pattern found at bytes [beg, end) of str: a match
   with no groups, whose Regexp is built only if `$~` is read. */
void sp_re_set_lit_match(const char *str, sp_int beg, sp_int end) {
  sp_re_caps[0] = (int)beg; sp_re_caps[1] = (int)end;
  sp_re_last_pat = NULL;
  sp_re_set_captures(str, sp_re_caps, 1);
  sp_re_last_lit = 1;
}

/* $` -- everything before the last match. */
const char *sp_re_pre_match(void) {
  if (sp_re_match_pre || sp_re_pp_span[0] < 0 || !sp_re_last_str) return sp_re_match_pre;
  int n = sp_re_pp_span[0];
  char *pre = sp_str_alloc_raw(n + 1);
  memcpy(pre, sp_re_last_str, n); pre[n] = 0;
  sp_str_set_len(pre, (size_t)n);
  sp_str_set_len(pre, (size_t)n);
  sp_re_match_pre = pre;
  return pre;
}

/* $\' -- everything after the last match. */
const char *sp_re_post_match(void) {
  if (sp_re_match_post || sp_re_pp_span[1] < 0 || !sp_re_last_str) return sp_re_match_post;
  int n = (int)sp_str_byte_len(sp_re_last_str) - sp_re_pp_span[1];
  if (n < 0) n = 0;
  char *post = sp_str_alloc_raw(n + 1);
  memcpy(post, sp_re_last_str + sp_re_pp_span[1], n); post[n] = 0;
  sp_str_set_len(post, (size_t)n);
  sp_str_set_len(post, (size_t)n);
  sp_re_match_post = post;
  return post;
}
sp_int sp_re_match(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str);
  if (!str) return -1;
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int ncaps = 32;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps, sp_str_is_binary(str));
  if (n > 0) { sp_re_last_pat = pat; sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0]; }
  /* Issue #848: clear backrefs on no-match so a subsequent `$1`
     reads as nil rather than the previous match's group. */
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  sp_re_last_str = NULL;
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  sp_re_pp_span[0] = sp_re_pp_span[1] = -1;
  return -1;
}
/* Like sp_re_match, but search from byte offset `pos` in the FULL string so a
   zero-width anchor (`\b`, a lookbehind) sees the preceding context -- passing
   `str + pos` instead would make every position look like a string start (the
   gsub/sub-with-block scan loop bug, #2910). Returns the match start relative
   to `pos` (so the caller's `str + pos` arithmetic and the `< 0` no-match check
   are unchanged); sp_re_caps stay full-string-relative for capture extraction. */
sp_int sp_re_match_at(mrb_regexp_pattern *pat, const char *str, sp_int pos) {SP_GC_ROOT_STR(str);
  if (!str) return -1;
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int ncaps = 32;
  int n = re_exec(pat, str, slen, pos, sp_re_caps, ncaps, sp_str_is_binary(str));
  if (n > 0) { sp_re_last_pat = pat; sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0] - pos; }
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  sp_re_last_str = NULL;
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  sp_re_pp_span[0] = sp_re_pp_span[1] = -1;
  return -1;
}
/* sp_re_match_at for the turns of gsub / sub with a block: a miss leaves the
   registers at the loop's last match, which is what `$~` reads once the call
   returns. The loop clears them before its first turn, so a subject with no
   match leaves nil. */
sp_int sp_re_match_next(mrb_regexp_pattern *pat, const char *str, sp_int pos) {SP_GC_ROOT_STR(str);
  if (!str) return -1;
  int caps[64];
  int n = re_exec(pat, str, (int64_t)sp_str_byte_len(str), pos, caps, 64, sp_str_is_binary(str));
  if (n <= 0 || caps[0] < 0) return -1;
  sp_re_set_last_match(pat, str, caps, n);
  return sp_re_caps[0] - pos;
}
/* MatchData#inspect: CRuby's #<MatchData "full" 1:"g1" ...> (named groups
   render by name; unmatched groups render nil). */
const char *sp_MatchData_inspect(sp_MatchData *m) {SP_GC_ROOT(m);
  if (!m) return SPL("nil");
  sp_String *b = sp_String_new("#<MatchData "); SP_GC_ROOT(b);
  sp_String_append(b, sp_str_inspect(sp_str_substr(m->source + m->caps[0], 0, m->caps[1] - m->caps[0])));
  for (int g = 1; g < m->ncap; g++) {
    const char *gname = re_group_name(m->pat, g);
    sp_String_append(b, " ");
    if (gname) { sp_String_append(b, gname); }
    else {
      char num[16]; snprintf(num, sizeof num, "%d", g);
      sp_String_append(b, num);
    }
    sp_String_append(b, ":");
    if (m->caps[g * 2] < 0) sp_String_append(b, "nil");
    else sp_String_append(b, sp_str_inspect(sp_str_substr(m->source + m->caps[g * 2], 0, m->caps[g * 2 + 1] - m->caps[g * 2])));
  }
  sp_String_append(b, ">");
  return sp_str_dup(sp_String_cstr(b));
}
/* s[/re/] = val: replace the first match's byte span with val (taken
   literally, no template expansion); no match raises IndexError. */
const char *sp_str_splice_re(mrb_regexp_pattern *pat, const char *s, const char *val) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(val);
  if (!s) s = "";
  if (!val) val = "";
  int64_t slen = (int64_t)sp_str_byte_len(s);
  int caps[2];
  int n = re_exec(pat, s, slen, 0, caps, 2, sp_str_is_binary(s));
  if (n <= 0) { sp_raise_cls("IndexError", "regexp not matched"); return s; }
  return sp_sprintf("%.*s%s%s", (int)caps[0], s, val, s + caps[1]);
}
/* String#slice!(regexp): the removed match (or NULL when unmatched), with
   the receiver's remainder written through rest_out and the match
   registers set (cleared on no-match, like sp_re_match). */
const char *sp_str_slice_re(mrb_regexp_pattern *pat, const char *s, const char **rest_out) {SP_GC_ROOT_STR(s);
  if (!s) s = &("\xff" "")[1];  /* header-safe empty: s flows to sp_str_byteslice -> sp_str_byte_len(s[-1]) */
  int64_t slen = (int64_t)sp_str_byte_len(s);
  int n = re_exec(pat, s, slen, 0, sp_re_caps, 32, sp_str_is_binary(s));
  if (n <= 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL;
    sp_re_match_str = NULL;
    sp_re_match_pre = NULL;
    sp_re_match_post = NULL;
    if (rest_out) *rest_out = s;
    return NULL;
  }
  int mb = sp_re_caps[0], me = sp_re_caps[1];
  sp_re_set_captures(s, sp_re_caps, n / 2);
  const char *m = sp_str_byteslice(s, mb, me - mb);
  SP_GC_ROOT_STR(m);
  if (rest_out) *rest_out = sp_sprintf("%.*s%s", mb, s, s + me);
  return m;
}
sp_int sp_re_rindex(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[2];
  int64_t pos = 0;
  sp_int last = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2, sp_str_is_binary(str));
    if (n <= 0) break;
    last = caps[0];
    /* rindex keys on the rightmost match START (MRI reverse search): step
       one past this start so a later-starting shorter match wins ("aaa"
       rindex /a+/ is 2, not 0). Advancing past the whole match skipped it. */
    pos = caps[0] + 1;
  }
  /* the result is a character index; caps[] holds byte offsets */
  return last < 0 ? -1 : sp_str_count_chars(str, (size_t)last);
}
sp_StrArray *sp_re_rpartition(mrb_regexp_pattern *pat, const char *str) {
  SP_GC_ROOT_STR(str);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[2];
  int64_t pos = 0;
  sp_int ms = -1, me = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2, sp_str_is_binary(str));
    if (n <= 0) break;
    ms = caps[0]; me = caps[1];
    /* rpartition keys on the rightmost match START (MRI reverse search),
       so step one past this start to look for a later-starting match. */
    pos = caps[0] + 1;
  }
  sp_StrArray *r = sp_StrArray_new();
  SP_GC_ROOT(r);
  if (ms < 0) {
    /* new Strings, as CRuby's: not the receiver, nor one shared "" */
    sp_StrArray_push(r, sp_str_dup(sp_str_empty));
    sp_StrArray_push(r, sp_str_dup(sp_str_empty));
    sp_StrArray_push(r, sp_str_dup(str));
    return r;
  }
  char *before = sp_str_alloc_raw(ms + 1);
  memcpy(before, str, ms); before[ms] = 0;
  sp_str_set_len(before, (size_t)ms);
  sp_StrArray_push(r, before);
  int mlen = (int)(me - ms);
  char *mid = sp_str_alloc_raw(mlen + 1);
  memcpy(mid, str + ms, mlen); mid[mlen] = 0;
  sp_str_set_len(mid, (size_t)mlen);
  sp_StrArray_push(r, mid);
  int alen = (int)(slen - me);
  char *after = sp_str_alloc_raw(alen + 1);
  memcpy(after, str + me, alen); after[alen] = 0;
  sp_str_set_len(after, (size_t)alen);
  sp_StrArray_push(r, after);
  return r;
}
sp_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str) {
  if (!str) return FALSE;
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[2];
  return re_exec(pat, str, slen, 0, caps, 2, sp_str_is_binary(str)) > 0;
}
sp_bool sp_re_match_p_at(mrb_regexp_pattern *pat, const char *str, sp_int pos) {
  if (!str) return FALSE;
  int64_t slen = (int64_t)sp_str_byte_len(str);
  if (pos < 0) pos += slen;
  if (pos < 0 || pos > slen) return FALSE;
  int caps[2];
  return re_exec(pat, str, slen, (sp_int)pos, caps, 2, sp_str_is_binary(str)) > 0;
}
/* Regexp#=== on a boxed operand (a case/when arm, an explicit ===). Only a
   String (plain or shared-mutable handle) or a Symbol can match; a match
   updates the $~ registers like =~. Any other operand answers false and
   clears the registers (CRuby sets the backref to nil there). */
sp_bool sp_re_case_eq(mrb_regexp_pattern *pat, sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  const char *s = NULL;
  if (v.tag == SP_TAG_STR) s = v.v.s ? v.v.s : "";
  else if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p)
    s = sp_String_cstr((sp_String *)v.v.p);
  else if (v.tag == SP_TAG_SYM && sp_sym_name_fn)
    s = sp_sym_name_fn((sp_sym)v.v.i);
  if (!s) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL;
    sp_re_match_str = NULL;
    sp_re_match_pre = NULL;
    sp_re_match_post = NULL;
    return FALSE;
  }
  return sp_re_match(pat, s) >= 0;
}
/* The pattern behind a boxed value: a Regexp box is one already, and a String
   is compiled the way `str.match?("b")` compiles its argument. Answers NULL for
   anything else. Used by the poly-receiver match forms below (#3961). */
mrb_regexp_pattern *sp_poly_as_pattern(sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_REGEX) return (mrb_regexp_pattern *)v.v.p;
  const char *s = NULL;
  if (v.tag == SP_TAG_STR) s = v.v.s ? v.v.s : "";
  else if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p)
    s = sp_String_cstr((sp_String *)v.v.p);
  if (!s) return NULL;
  return re_compile(s, (int64_t)sp_str_byte_len(s), 0);
}
/* The subject string behind a boxed value (a plain string, a shared handle, a
   Symbol); NULL when the value is not one. */
static const char *sp_poly_subject(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s ? v.v.s : SPL("");
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_STRBUF && v.v.p)
    return sp_String_cstr((sp_String *)v.v.p);
  if (v.tag == SP_TAG_SYM && sp_sym_name_fn) return sp_sym_name_fn((sp_sym)v.v.i);
  return NULL;
}
/* `a.match?(b)` / `a.match(b)` / `a =~ b` where either operand only reads poly:
   whichever side is the Regexp is the pattern, and the other is the subject --
   the same rule CRuby applies, and the reason both `re.match?(s)` and
   `s.match?(re)` work. A pattern that cannot be built answers no match. */
static int sp_poly_match_pair(sp_RbVal a, sp_RbVal b,
                              mrb_regexp_pattern **pat_out, const char **str_out) {
  sp_RbVal pv, sv;
  if (b.tag == SP_TAG_OBJ && b.cls_id == SP_BUILTIN_REGEX) { pv = b; sv = a; }
  else if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_REGEX) { pv = a; sv = b; }
  /* Neither side is a Regexp, which is `str.match("x")`: CRuby builds the
     pattern from the ARGUMENT and matches the receiver against it. Taking the
     receiver as the pattern matched "x" against /xyz/ and answered no match
     for every such call. */
  else { pv = b; sv = a; }
  const char *s = sp_poly_subject(sv);
  if (!s) return 0;
  mrb_regexp_pattern *p = sp_poly_as_pattern(pv);
  if (!p) return 0;
  *pat_out = p; *str_out = s;
  return 1;
}
sp_bool sp_poly_match_p(sp_RbVal a, sp_RbVal b) {SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);
  mrb_regexp_pattern *p; const char *s;
  if (!sp_poly_match_pair(a, b, &p, &s)) return FALSE;
  return sp_re_match_p(p, s);
}
/* match?(pattern, pos) with a boxed operand: the same pair, from character
   position pos of the subject */
sp_bool sp_poly_match_p_at(sp_RbVal a, sp_RbVal b, sp_int pos) {SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);
  mrb_regexp_pattern *p; const char *s;
  if (!sp_poly_match_pair(a, b, &p, &s)) return FALSE;
  return sp_str_re_match_p_at(p, s, pos);
}
sp_MatchData *sp_poly_match_data(sp_RbVal a, sp_RbVal b) {SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);
  mrb_regexp_pattern *p; const char *s;
  if (!sp_poly_match_pair(a, b, &p, &s)) return NULL;
  return sp_re_matchdata(p, s);
}
sp_oint sp_poly_match_index(sp_RbVal a, sp_RbVal b) {SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(b);
  mrb_regexp_pattern *p; const char *s;
  if (!sp_poly_match_pair(a, b, &p, &s)) return sp_oint_nil();
  sp_int r = sp_re_match(p, s);
  return r < 0 ? sp_oint_nil() : sp_oint_of(r);
}
void sp_re_expand_rep(const mrb_regexp_pattern *pat,
                             char **out_io, size_t *olen_io, size_t *cap_io,
                             const char *rep, size_t rlen,
                             const char *src, int *caps, int ncaps) {
  size_t olen = *olen_io;
  char *out = *out_io;
  size_t cap = *cap_io;
  size_t i = 0;
  while (i < rlen) {
    char c = rep[i];
    if (c == '\\' && i + 1 < rlen) {
      char d = rep[i+1];
      if ((d >= '0' && d <= '9') || d == '&' || d == '+') {
        int gi = (d == '&') ? 0 : (d == '+') ? 0 : (d - '0');
        /* A pattern that names a group turns `\1` through `\9` off, the same
           rule that stops a plain `(...)` from taking a number there: the
           number a named group answers to for md[1] is not one a replacement
           may spend, and `\k<name>` is what reaches it. `\0` is the whole
           match, which naming a group does not touch, and neither do `\&` and
           `\+`. A literal String pattern hands in no pattern and has no group
           for a number to reach either way. */
        if (d >= '1' && d <= '9' && pat && re_num_named(pat) > 0) { i += 2; continue; }
        if (d == '+') {
          /* \+: the highest-numbered group that participated in the match;
             none participating expands to "" (gi stays 0 with caps[0] the
             whole match -- so scan for a real group first) */
          gi = -1;
          for (int g = 1; (g * 2) + 1 < ncaps; g++)
            if (caps[g*2] >= 0 && caps[(g*2)+1] >= 0) gi = g;
          if (gi < 0) { i += 2; continue; }
        }
        if ((gi*2) + 1 < ncaps && caps[gi*2] >= 0 && caps[(gi*2)+1] >= 0) {
          int g_len = caps[(gi*2)+1] - caps[gi*2];
          if (olen + g_len + 1 >= cap) { cap = ((olen + g_len) * 2) + 64; out = (char*)realloc(out, cap); }
          memcpy(out+olen, src + caps[gi*2], g_len);
          olen += g_len;
        }
        i += 2;
        continue;
      }
      else if (d == '`' || d == '\'') {
        /* \` is the text before the match, \' the text after it (#3550) */
        int seg_beg = (d == '`') ? 0 : caps[1];
        int seg_end = (d == '`') ? caps[0] : (int)strlen(src);
        if (seg_beg >= 0 && seg_end >= seg_beg) {
          int seg_len = seg_end - seg_beg;
          if (olen + seg_len + 1 >= cap) { cap = ((olen + seg_len) * 2) + 64; out = (char*)realloc(out, cap); }
          memcpy(out + olen, src + seg_beg, (size_t)seg_len);
          olen += (size_t)seg_len;
        }
        i += 2;
        continue;
      }
      else if (d == 'k' && i + 2 < rlen && rep[i+2] == '<') {
       /* \k<name>: named backreference. The name resolves against the
          pattern; an unknown or empty name raises IndexError (mirroring
          md[:name]), and a group that did not participate expands to "".
          Only the angle-bracket form is a replacement backref -- \k'name'
          is regex-only syntax, so it is left literal (handled by the
          default copy below when no closing '>' is found). */
        size_t j = i + 3;
        while (j < rlen && rep[j] != '>') j++;
        if (j < rlen) {
          size_t nlen = j - (i + 3);
          /* Group names are short; keep them on the stack and only fall back
             to malloc for an exceptionally long name. */
          char name_buf[64];
          char *name = name_buf;
          if (nlen >= sizeof(name_buf)) {
            name = (char*)malloc(nlen + 1);
            if (!name) { perror("malloc"); exit(1); }
          }
          memcpy(name, rep + i + 3, nlen);
          name[nlen] = '\0';
          int gi = re_named_group(pat, name);
          if (gi < 0) {
           /* Build the message before freeing the name, then release the
              scratch buffer: sp_raise_cls longjmps, so the caller's free()
              never runs and `out` (post-realloc) would otherwise leak. */
            const char *msg = sp_sprintf("undefined group name reference: %s", name);
            if (name != name_buf) free(name);
            free(out);
            sp_raise_cls("IndexError", msg);
          }
          if (name != name_buf) free(name);
          if ((gi*2) + 1 < ncaps && caps[gi*2] >= 0 && caps[(gi*2)+1] >= 0) {
            int g_len = caps[(gi*2)+1] - caps[gi*2];
            if (olen + g_len + 1 >= cap) { cap = ((olen + g_len) * 2) + 64; out = (char*)realloc(out, cap); }
            memcpy(out+olen, src + caps[gi*2], g_len);
            olen += g_len;
          }
          i = j + 1;
          continue;
        }
      }
else if (d == '\\') {
        if (olen + 1 >= cap) { cap = (cap * 2) + 64; out = (char*)realloc(out, cap); }
        out[olen++] = '\\';
        i += 2;
        continue;
      }
    }
    if (olen + 1 >= cap) { cap = (cap * 2) + 64; out = (char*)realloc(out, cap); }
    out[olen++] = c;
    i++;
  }
  *out_io = out; *olen_io = olen; *cap_io = cap;
}
const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep) {SP_GC_ROOT_STR(str);SP_GC_ROOT_STR(rep);if(!str)sp_nil_recv("gsub");
  int64_t slen = (int64_t)sp_str_byte_len(str); size_t rlen = sp_str_byte_len(rep);
  size_t cap = (slen * 2) + (rlen * 4) + 64;
 /* Build into a plain malloc scratch: the buffer is grown with realloc
    here and inside sp_re_expand_rep, which is only valid on a real
    malloc base (a sp_str body pointer is offset past its header). The
    final string is allocated at the exact length below. */
  char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  int lastcaps[64], lastn = 0;   /* the last match, for `$~` */
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64, sp_str_is_binary(str));
    if (n <= 0 || caps[0] < 0) break;
    sp_re_sub_matched = 1;
    if (sp_re_track_last) { lastn = n > 64 ? 64 : n; memcpy(lastcaps, caps, sizeof(int) * (size_t)lastn); }
    size_t before = caps[0] - pos;
    if (olen+before+rlen >= cap) { cap = ((olen+before+rlen)*2)+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, before); olen += before;
    sp_re_expand_rep(pat, &out, &olen, &cap, rep, rlen, str, caps, n);
    if (caps[0] == caps[1]) {
 /* Zero-width match (/^/, /$/, /\b/, an empty pattern): Ruby inserts
    the replacement before the char at this position, keeps that char,
    and advances past it. Copy the char and step by one so the scan
    makes progress without dropping it or spinning on the same spot. */
      if (caps[1] < slen) {
        if (olen+1 >= cap) { cap = (olen*2)+64; out = (char*)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    }
else {
      pos = caps[1];
    }
  }
 /* pos can land at slen+1 after a zero-width match at the end; guard the
    tail copy so `slen - pos` doesn't underflow size_t. */
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen+rest+1 >= cap) { cap = olen+rest+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, rest); olen += rest;
  }
  if (sp_re_track_last) sp_re_set_last_match(pat, str, lastcaps, lastn);
 /* Emit a string sized to exactly the bytes written (sp_str_alloc sets
    the length and null-terminates); release the scratch. */
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep) {SP_GC_ROOT_STR(str);SP_GC_ROOT_STR(rep);if(!str)sp_nil_recv("sub");
  int64_t slen = (int64_t)sp_str_byte_len(str); size_t rlen = sp_str_byte_len(rep);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64, sp_str_is_binary(str));
  /* no match: a new String all the same, as CRuby's sub and as gsub here */
  if (n <= 0 || caps[0] < 0) { if (sp_re_track_last) sp_re_clear_last_match(); return sp_str_dup(str); }
  sp_re_sub_matched = 1;
  /* Issue #855: expand `\1`..`\9` / `\&` from rep against caps. */
  size_t cap = caps[0] + (rlen * 4) + (slen - caps[1]) + 64;
 /* malloc scratch: sp_re_expand_rep and the tail grow it with realloc,
    which needs a real malloc base. Exact-sized string emitted below. */
  char *out = (char *)malloc(cap);
  memcpy(out, str, caps[0]);
  size_t olen = caps[0];
  sp_re_expand_rep(pat, &out, &olen, &cap, rep, rlen, str, caps, n);
  size_t rest = slen - caps[1];
  if (olen + rest + 1 >= cap) { cap = olen + rest + 64; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+caps[1], rest); olen += rest;
  if (sp_re_track_last) sp_re_set_last_match(pat, str, caps, n);
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str) {
  SP_GC_ROOT_STR(str);
  if (!str) sp_nil_recv("scan");   /* a nullable String's nil receiver */
  sp_StrArray *arr = sp_StrArray_new();
  SP_GC_ROOT(arr);
  int64_t slen = (int64_t)sp_str_byte_len(str); int64_t pos = 0; int caps[64];
  int lastcaps[64], lastn = 0;   /* the last match, for `$~` */
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64, sp_str_is_binary(str));
    if (n <= 0 || caps[0] < 0) break;
    if (sp_re_track_last) { lastn = n > 64 ? 64 : n; memcpy(lastcaps, caps, sizeof(int) * (size_t)lastn); }
    int len = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(len+1); memcpy(m, str+caps[0], len); m[len] = 0;
    sp_str_set_len(m, (size_t)len);
    sp_StrArray_push(arr, m);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  if (sp_re_track_last) sp_re_set_last_match(pat, str, lastcaps, lastn);
  return arr;
}
/* Forward decl from the regexp engine (lib/regexp/re_utf8.c). */
int re_utf8_charlen(const char *s, const char *end);

/* String#split(regexp[, limit]). Mirrors CRuby / upstream mruby-regexp
   string_regexp.rb#split: a zero-width match steps past one whole (multibyte-
   aware) character and never emits an empty leading field; capture groups are
   spliced between the surrounding fields (unmatched optional groups are
   omitted, like md[i].nil?); and with the default limit (0) trailing empty
   fields are stripped. limit > 0 caps the field count (the last field is the
   unsplit remainder); limit < 0 keeps trailing empties. An empty subject is
   always []. */
static void split_push_slice(sp_StrArray *arr, const char *str, int64_t from, int64_t to) {
  int len = (int)(to - from);
  char *m = sp_str_alloc_raw(len + 1);
  memcpy(m, str + from, len); m[len] = 0;
  sp_str_set_len(m, (size_t)len);
  sp_StrArray_push(arr, m);
}

sp_StrArray *sp_re_split_limit(mrb_regexp_pattern *pat, const char *str, sp_int limit) {SP_GC_ROOT_STR(str);if(!str)sp_nil_recv("split");
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)sp_str_byte_len(str);

  /* limit == 1: the whole string is the single field; "" splits to []. */
  if (limit == 1) {
    if (slen > 0) split_push_slice(arr, str, 0, slen);
    return arr;
  }

  int64_t field_start = 0, search_pos = 0, count = 0;
  int caps[64];
  while (search_pos <= slen) {
    if (limit > 0 && count >= limit - 1) {
      split_push_slice(arr, str, field_start, slen);
      return arr;
    }
    int n = re_exec(pat, str, slen, search_pos, caps, 64, sp_str_is_binary(str));
    if (n <= 0 || caps[0] < 0) break;
    int64_t match_start = caps[0], match_end = caps[1];

    if (match_start == match_end) {
      /* zero-width: advance past one whole character so multibyte text is
         not split between bytes. */
      if (match_end < slen) {
        search_pos = match_end + re_utf8_charlen(str + match_end, str + slen);
      }
      else {
        search_pos = match_end + 1;
      }
      if (match_start == field_start) continue;  /* nothing to emit yet */
    }

    split_push_slice(arr, str, field_start, match_start);
    count++;
    field_start = match_end;
    if (match_start != match_end) search_pos = match_end;

    /* Splice captured groups (caps[0] is the whole match; groups 1.. follow).
       Unmatched optional groups are skipped, matching CRuby. */
    for (int gi = 1; (gi * 2) + 1 < n; gi++) {
      if (caps[gi*2] >= 0 && caps[(gi*2)+1] >= 0) {
        split_push_slice(arr, str, caps[gi*2], caps[(gi*2)+1]);
      }
    }
  }

  /* Trailing field (omitted for an empty subject, or when stripped below). */
  if (slen > 0 && field_start <= slen && (field_start < slen || limit != 0)) {
    split_push_slice(arr, str, field_start, slen);
  }

  /* Default limit strips trailing empty fields. */
  if (limit == 0) {
    while (arr->len > 0 && arr->data[arr->len - 1][0] == '\0') arr->len--;
  }
  return arr;
}

sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str);if(!str)sp_nil_recv("split");
  return sp_re_split_limit(pat, str, 0);
}
sp_oint sp_re_rindex_opt(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str); sp_int n = sp_re_rindex(pat, str); return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
sp_RbVal sp_re_rindex_poly(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str); sp_int n = sp_re_rindex(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_re_index_poly(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str); sp_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(sp_str_byte_to_char(str, n)); }  /* char offset (#3056) */
/* String#index(regexp, start): first match at or after char position `start`,
   as a char index -- nil on miss / out-of-range (matching
   sp_str_index_from_opt's ABI). */
/* String#byteindex(regexp[, start]): first match at or after BYTE offset
   `start`, answered as a byte offset (nil on miss). */
sp_oint sp_re_byteindex_opt(mrb_regexp_pattern *pat, const char *str, sp_int start) {
  if (!str) return sp_oint_nil();
  sp_int bl = (sp_int)sp_str_byte_len(str);
  if (start < 0) start += bl;
  if (start < 0 || start > bl) return sp_oint_nil();
  int caps[64];
  int n = re_exec(pat, str, (int64_t)bl, start, caps, 64, sp_str_is_binary(str));
  if (n <= 0 || caps[0] < 0) return sp_oint_nil();
  return sp_oint_of((sp_int)caps[0]);
}
/* String#byterindex(regexp[, start]): last match starting at or before BYTE
   offset `start`, answered as a byte offset (nil on miss). */
sp_oint sp_re_byterindex_opt(mrb_regexp_pattern *pat, const char *str, sp_int start) {
  if (!str) return sp_oint_nil();
  sp_int bl = (sp_int)sp_str_byte_len(str);
  if (start < 0) start += bl;
  if (start < 0) return sp_oint_nil();
  if (start > bl) start = bl;
  /* the match STARTING latest wins (a match at 3 beats a longer one at 2),
     so probe each start position from `start` downward */
  int caps[2];
  for (sp_int p = start; p >= 0; p--) {
    int n = re_exec(pat, str, (int64_t)bl, p, caps, 2, sp_str_is_binary(str));
    if (n > 0 && caps[0] == (int)p) return sp_oint_of(p);
  }
  return sp_oint_nil();
}
sp_oint sp_re_index_from_opt(mrb_regexp_pattern *pat, const char *str, sp_int start) {SP_GC_ROOT_STR(str);
  if (!str) return sp_oint_nil();
  sp_int cl = sp_str_length(str);
  if (start < 0) start += cl;
  if (start < 0 || start > cl) return sp_oint_nil();
  size_t boff = sp_utf8_byte_offset(str, start);
  int caps[64];
  int n = re_exec(pat, str, (int64_t)sp_str_byte_len(str), (sp_int)boff, caps, 64, sp_str_is_binary(str));
  if (n <= 0 || caps[0] < 0) return sp_oint_nil();
  return sp_oint_of(sp_str_count_chars(str, (size_t)caps[0]));
}
/* String#rindex(regexp, start): last match whose start is at or before char
   position `start`, as a char index (nil on miss). */
sp_oint sp_re_rindex_from_opt(mrb_regexp_pattern *pat, const char *str, sp_int start) {SP_GC_ROOT_STR(str);
  if (!str) return sp_oint_nil();
  sp_int cl = sp_str_length(str);
  if (start < 0) start += cl;
  if (start < 0) return sp_oint_nil();
  if (start > cl) start = cl;
  size_t limit = sp_utf8_byte_offset(str, start);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[2];
  int64_t pos = 0; sp_int last = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2, sp_str_is_binary(str));
    if (n <= 0 || caps[0] < 0) break;
    if ((size_t)caps[0] > limit) break;
    last = caps[0];
    int64_t next = caps[1]; if (next <= pos) next = pos + 1; pos = next;
  }
  return last < 0 ? sp_oint_nil() : sp_oint_of(sp_str_count_chars(str, (size_t)last));
}
sp_RbVal sp_re_match_poly(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str); sp_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(sp_str_byte_to_char(str, n)); }  /* char offset (#3056) */
/* Value of the named group `name` from the most recent match registers (set by
   sp_re_match / sp_re_match_poly). NULL (nil) when the last match failed, the
   name is unknown, or the group did not participate. Used by `/(?<n>..)/ =~ s`
   named-capture local binding (MatchWriteNode). */
/* A name the pattern has no group for is CRuby's IndexError; the callers
   ask only once the pattern matched, so a failed match stays nil. */
const char *sp_re_named_capture(const mrb_regexp_pattern *pat, const char *name) {
  if (!pat || !name || !sp_re_last_str) return NULL;
  int g = re_named_group(pat, name);
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %s", name));
  if ((g * 2) + 1 >= 64) return NULL;
  int b = sp_re_caps[g * 2], e = sp_re_caps[(g * 2) + 1];
  /* e < b also covers e < 0 once b >= 0; guards against a malformed register
     state yielding a negative len that would cast to a huge size_t. */
  if (b < 0 || e < b) return NULL;
  int len = e - b;
  char *out = sp_str_alloc(len);
  memcpy(out, sp_re_last_str + b, len);
  return out;
}
/* `a|b`, built byte-wise: sp_sprintf's %s ends at an embedded NUL, so a
   branch carrying one joined short. Used by Regexp.union, both the runtime
   array form below and the fixed-argument form the emitter builds. */
const char *sp_re_alt_join(const char *a, const char *b) {SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);
  size_t al = sp_str_byte_len(a), bl = sp_str_byte_len(b);
  char *buf = sp_str_alloc_raw(al + 1 + bl + 1);
  memcpy(buf, a, al);
  buf[al] = '|';
  memcpy(buf + al + 1, b, bl);
  buf[al + 1 + bl] = 0;
  sp_str_set_len(buf, al + 1 + bl);
  return buf;
}

const char *sp_re_escape(const char *src) {SP_GC_ROOT_STR(src);
  /* the text may hold a NUL, and strlen would escape only the part before
     it -- which stayed invisible while a NUL-free prefix needed no escaping
     at all and the function returned `src` untouched */
  size_t i, in_len = sp_str_byte_len(src);
  size_t out_len = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      out_len += 2;
    }
else {
      out_len += 1;
    }
  }
  if (out_len == in_len) {
    return src;
  }
  char *buf = sp_str_alloc(out_len);
  size_t j = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      buf[j++] = '\\';
      /* a control character escapes to its LETTER form ("\n", not a backslash
         followed by a real newline), which is what CRuby produces (#3635) */
      buf[j++] = c == '\n' ? 'n' : c == '\t' ? 't' : c == '\r' ? 'r'
               : c == '\f' ? 'f' : c == '\v' ? 'v' : (char)c;
    }
else {
      buf[j++] = (char)c;
    }
  }
  buf[j] = 0;
  sp_str_set_len(buf, j);
  return buf;
}
/* Regexp.union over a runtime array (elements known only at run time, e.g. an
   Array held in a variable). Each element joins by alternation: a String is
   regexp-escaped, a Regexp contributes its #to_s form `(?on-off:src)` so its
   options survive. An empty array yields the never-matching /(?!)/. */
mrb_regexp_pattern *sp_re_union_array(sp_PolyArray *a) {
  if (!a || a->len == 0) return re_compile("(?!)", 4, 0);
  /* a lone Regexp is the answer itself, its source and flags as they are */
  if (a->len == 1) {
    sp_RbVal v0 = sp_PolyArray_get(a, 0);
    if (v0.tag == SP_TAG_OBJ && v0.cls_id == SP_BUILTIN_REGEX && v0.v.p) return (mrb_regexp_pattern *)v0.v.p;
  }
  const char *joined = NULL;
  for (sp_int i = 0; i < a->len; i++) {
    sp_RbVal v = sp_PolyArray_get(a, i);
    const char *part;
    if (v.tag == SP_TAG_STR) part = sp_re_escape(v.v.s ? v.v.s : "");
    else if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_REGEX) part = sp_re_to_s_str(v.v.p);
    else { sp_raise_cls("TypeError", "no implicit conversion of element into String or Regexp"); return NULL; }
    joined = (i == 0) ? part : sp_re_alt_join(joined, part);
  }
  return re_compile(joined, (int64_t)sp_str_byte_len(joined), 0);
}
sp_PolyArray *sp_re_scan_poly(mrb_regexp_pattern *pat, const char *str) {
  SP_GC_ROOT_STR(str);
  if (!str) sp_nil_recv("scan");
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int64_t pos = 0;
  int ncaps = 64;
  int caps[64];
  int lastcaps[64], lastn = 0;   /* the last match, for `$~` */
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, ncaps, sp_str_is_binary(str));
    if (n <= 0 || caps[0] < 0) break;
    if (sp_re_track_last) { lastn = n > ncaps ? ncaps : n; memcpy(lastcaps, caps, sizeof(int) * (size_t)lastn); }
    int pairs = (n > ncaps ? ncaps : n) / 2;
    if (pairs <= 1) {
      int len = caps[1] - caps[0];
      char *m = sp_str_alloc_raw(len + 1);
      memcpy(m, str + caps[0], len);
      m[len] = 0;
      sp_str_set_len(m, (size_t)len);
      sp_PolyArray_push(arr, sp_box_str(m));
    }
else {
      sp_PolyArray *row = sp_PolyArray_new();
      SP_GC_ROOT(row);
      for (int gi = 1; gi < pairs; gi++) {
        if (caps[gi * 2] >= 0 && caps[(gi * 2) + 1] >= 0) {
          int glen = caps[(gi * 2) + 1] - caps[gi * 2];
          char *gm = sp_str_alloc_raw(glen + 1);
          memcpy(gm, str + caps[gi * 2], glen);
          gm[glen] = 0;
          sp_str_set_len(gm, (size_t)glen);
          sp_PolyArray_push(row, sp_box_str(gm));
        }
else {
          sp_PolyArray_push(row, sp_box_nil());
        }
      }
      sp_PolyArray_push(arr, sp_box_poly_array(row));
    }
    pos = caps[1];
    if (caps[0] == caps[1]) pos++;
  }
  if (sp_re_track_last) sp_re_set_last_match(pat, str, lastcaps, lastn);
  return arr;
}
sp_PolyArray *sp_re_match_data(mrb_regexp_pattern *pat, const char *str) {
  SP_GC_ROOT_STR(str);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int ncaps = 64;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps, sp_str_is_binary(str));
  if (n <= 0 || sp_re_caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL;
    sp_re_match_str = NULL;
    sp_re_match_pre = NULL;
    sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > ncaps ? ncaps : n) / 2;
  sp_re_set_captures(str, sp_re_caps, pairs);
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  for (int i = 0; i < pairs; i++) {
    int start = sp_re_caps[i * 2];
    int end = sp_re_caps[(i * 2) + 1];
    if (start >= 0 && end >= start) {
      int len = end - start;
      char *buf = sp_str_alloc_raw(len + 1);
      memcpy(buf, str + start, len);
      buf[len] = 0;
      sp_str_set_len(buf, (size_t)len);
      sp_PolyArray_push(arr, sp_box_str(buf));
    }
else {
      sp_PolyArray_push(arr, sp_box_nil());
    }
  }
  return arr;
}
/* Regexp#inspect and #to_s, built byte-wise.
   These used to live beside the engine in lib/regexp, where the only string
   builder available is sp_sprintf -- whose %s ends at a NUL, so a pattern
   holding one rendered short. They belong on this side anyway: the engine is
   a port from mruby-regexp and these are spinel's own surface. */
static void sp_re_append_escaped_source(sp_String *b, void *vpat) {
  const char *src = sp_re_source(vpat);
  size_t n = sp_re_source_len(vpat);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)src[i];
    /* a literal `/` is escaped, unless it already is */
    if (c == '/' && (i == 0 || src[i - 1] != '\\')) {
      sp_String_append(b, "\\");
      sp_fd_append_len(b, src + i, 1);
      continue;
    }
    /* CRuby renders a byte that is neither printable nor one of the five
       whitespace controls as \xNN: measured, what it leaves raw is 0x09-0x0D,
       0x20-0x7E, and everything above ASCII (a multi-byte character stays
       itself). A NUL rendered raw would also end any C string the text is
       handed to. */
    if (c < 0x09 || (c > 0x0D && c < 0x20) || c == 0x7F) {
      char hex[8];
      snprintf(hex, sizeof hex, "\\x%02X", c);
      sp_String_append(b, hex);
      continue;
    }
    sp_fd_append_len(b, src + i, 1);
  }
}

const char *sp_re_inspect_str(void *vpat) {
  uint32_t f = sp_re_raw_flags(vpat);
  sp_String *b = sp_String_new(""); SP_GC_ROOT(b);
  sp_String_append(b, "/");
  sp_re_append_escaped_source(b, vpat);
  sp_String_append(b, "/");
  if (f & SP_RE_F_DOTALL)     sp_String_append(b, "m");
  if (f & SP_RE_F_IGNORECASE) sp_String_append(b, "i");
  if (f & SP_RE_F_EXTENDED)   sp_String_append(b, "x");
  return b->data;
}

const char *sp_re_to_s_str(void *vpat) {
  uint32_t f = sp_re_raw_flags(vpat);
  char on[4], off[4]; int no = 0, nf = 0;
  if (f & SP_RE_F_DOTALL) on[no++] = 'm'; else off[nf++] = 'm';
  if (f & SP_RE_F_IGNORECASE) on[no++] = 'i'; else off[nf++] = 'i';
  if (f & SP_RE_F_EXTENDED) on[no++] = 'x'; else off[nf++] = 'x';
  on[no] = 0; off[nf] = 0;
  sp_String *b = sp_String_new(""); SP_GC_ROOT(b);
  sp_String_append(b, "(?");
  sp_String_append(b, on);
  if (nf) { sp_String_append(b, "-"); sp_String_append(b, off); }
  sp_String_append(b, ":");
  sp_re_append_escaped_source(b, vpat);
  sp_String_append(b, ")");
  return b->data;
}

void sp_MatchData_scan(void *p) { sp_MatchData *m = (sp_MatchData *)p; if (m->source) sp_mark_string(m->source); }
/* One block for the struct and the positions it carries, sized to the match
   rather than to the widest one this engine allows. The single allocation is
   also what keeps the two from ever disagreeing about who owns which. */
static sp_MatchData *sp_md_alloc(int pairs) {
  size_t sz = sizeof(sp_MatchData) + (size_t)pairs * 2 * sizeof(int);
  return (sp_MatchData *)sp_gc_alloc(sz, NULL, sp_MatchData_scan);
}
sp_MatchData *sp_re_matchdata(mrb_regexp_pattern *pat, const char *str) {SP_GC_ROOT_STR(str);
  if (!str) return NULL;   /* Regexp#match(nil) is nil, not a walk off NULL (#3633) */
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64, sp_str_is_binary(str));
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = sp_md_alloc(pairs);
  m->source = str;
  m->ncap = pairs;
  m->pat = pat;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* String#match(/re/, pos) -- pos is a codepoint index (CRuby semantics). */
sp_MatchData *sp_re_matchdata_at(mrb_regexp_pattern *pat, const char *str, sp_int cpos) {SP_GC_ROOT_STR(str);
  if (!str) return NULL;
  sp_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return NULL;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)sp_str_byte_len(str);
  int caps[64];
  int n = re_exec(pat, str, slen, (sp_int)boff, caps, 64, sp_str_is_binary(str));
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = sp_md_alloc(pairs);
  m->source = str;
  m->ncap = pairs;
  m->pat = pat;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* group i substring, or NULL for a non-participating / out-of-range group */
const char *sp_MatchData_aref(sp_MatchData *m, sp_int i) {SP_GC_ROOT(m);
  if (!m) return NULL;
  /* A negative index reaches the CAPTURE groups only: m[-1] is the last one and
     m[-(ncap)] -- which would be the whole match -- is nil, as in CRuby (#3628). */
  if (i < 0) { i += m->ncap; if (i < 1) return NULL; }
  if (i >= m->ncap) return NULL;
  int s = m->caps[i * 2], e = m->caps[(i * 2) + 1];
  if (s < 0 || e < s) return NULL;
  int len = e - s;
  char *b = sp_str_alloc((size_t)len);
  memcpy(b, m->source + s, len);
  b[len] = 0;
  sp_str_set_len(b, (size_t)len);
  return b;
}
/* group by name (`md[:name]` / `md["name"]`): resolve the name to its capture
   group via the pattern, then return that group's substring (NULL if the name
   is unknown or the group did not participate). */
const char *sp_MatchData_aref_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name);
  if (!m || !name) return NULL;
  int g = re_named_group(m->pat, name);
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %s", name));
  return sp_MatchData_aref(m, g);
}
/* `md.names`: the capture names in declaration order. */
/* Regexp#names on the pattern itself: named groups in declaration order. */
/* Regexp.linear_time?: a backreference (\1..\9, \k<name>, \g<name>) defeats
   the linear-time matcher. Inside a character class those are not
   backreferences, so track class membership (#3684). */
sp_bool sp_re_src_linear_time(const char *src) {SP_GC_ROOT_STR(src);
  if (!src) return TRUE;
  int in_class = 0;
  for (const char *s = src; *s; s++) {
    if (*s == '\\' && s[1]) {
      char n = s[1];
      if (!in_class && ((n >= '1' && n <= '9') || n == 'k' || n == 'g')) return FALSE;
      s++;
      continue;
    }
    if (in_class) { if (*s == ']') in_class = 0; }
    else if (*s == '[') in_class = 1;
  }
  return TRUE;
}

sp_StrArray *sp_Regexp_names(const mrb_regexp_pattern *pat) {
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  if (!pat) return a;
  int n = re_num_named(pat);
  for (int i = 0; i < n; i++) {
    const char *nm = re_named_name(pat, i, NULL);
    /* a name reused across alternatives is one name, however many groups
       carry it (#3682) */
    if (nm && !sp_StrArray_include(a, nm)) sp_StrArray_push(a, sp_str_dup(nm));
  }
  return a;
}

/* MatchData#string: the frozen match subject. */
const char *sp_MatchData_string(sp_MatchData *m) {
  return m ? m->source : NULL;
}

sp_StrArray *sp_MatchData_names(sp_MatchData *m) {SP_GC_ROOT(m);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  if (!m) return a;
  int n = re_num_named(m->pat);
  for (int i = 0; i < n; i++) {
    const char *nm = re_named_name(m->pat, i, NULL);
    if (nm && !sp_StrArray_include(a, nm)) sp_StrArray_push(a, sp_str_dup(nm));
  }
  return a;
}
sp_int sp_MatchData_length(sp_MatchData *m) { return m ? m->ncap : 0; }
/* MatchData#== / #eql?: same match over the same source with identical capture
   spans (#2529). */
sp_bool sp_MatchData_eq(sp_MatchData *a, sp_MatchData *b) {
  if (a == b) return TRUE;
  if (!a || !b || a->ncap != b->ncap) return FALSE;
  if (a->source && b->source) { if (strcmp(a->source, b->source) != 0) return FALSE; }
  else if (a->source != b->source) return FALSE;
  for (int i = 0; i < a->ncap * 2; i++) if (a->caps[i] != b->caps[i]) return FALSE;
  return TRUE;
}
/* A content-based hash over the same fields sp_MatchData_eq compares, so equal
   MatchData hash alike and different matches (usually) do not (#3014). */
sp_int sp_MatchData_hash(sp_MatchData *m) {
  if (!m) return 0;
  uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
  if (m->source) for (const char *p = m->source; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
  h ^= (uint64_t)m->ncap; h *= 1099511628211ULL;
  for (int i = 0; i < m->ncap * 2; i++) { h ^= (uint64_t)(uint32_t)m->caps[i]; h *= 1099511628211ULL; }
  return (sp_int)(h >> 1);   /* non-negative */
}
/* MatchData#[range]: the groups selected by a Range of indices (#2532). */
sp_PolyArray *sp_MatchData_aref_range(sp_MatchData *m, sp_int beg, sp_int end, int excl) {SP_GC_ROOT(m);
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  if (!m) return a;
  sp_int n = m->ncap;
  /* a beginless or endless bound carries the range sentinel, which the
     negative-index fixup below turned into a wild offset (#3628) */
  if (beg == INTPTR_MIN) beg = 0;
  if (end == INTPTR_MAX) { end = n - 1; excl = 0; }
  if (beg < 0) beg += n;
  if (end < 0) end += n;
  sp_int last = excl ? end - 1 : end;
  if (beg < 0 || beg > n) return NULL;   /* nil in Ruby */
  for (sp_int i = beg; i <= last && i < n; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(a, g ? sp_box_str(g) : sp_box_nil());
  }
  return a;
}
/* MatchData#[start, length]: an Array of `length` groups from `start` (nil for
   a group that did not participate), like Array#[start, length] (#2507). */
sp_PolyArray *sp_MatchData_aref_len(sp_MatchData *m, sp_int start, sp_int len) {SP_GC_ROOT(m);
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  if (!m) return a;
  if (start < 0) start += m->ncap;
  if (start < 0 || start > m->ncap || len < 0) return NULL;   /* nil in Ruby */
  for (sp_int i = start; i < start + len && i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(a, g ? sp_box_str(g) : sp_box_nil());
  }
  return a;
}
/* char offset of a byte position within source */
sp_oint sp_md_char_off(sp_MatchData *m, int byteoff) {SP_GC_ROOT(m);
  if (byteoff < 0) return sp_oint_nil();
  return sp_oint_of(sp_str_count_chars(m->source, (size_t)byteoff));
}
/* An index outside the match's groups is CRuby's IndexError, not nil (#3626). */
static void sp_md_check_index(sp_MatchData *m, sp_int i) {
  if (!m || i < 0 || i >= m->ncap)
    sp_raise_cls("IndexError", sp_sprintf("index %lld out of matches", (long long)i));
}
sp_oint sp_MatchData_begin(sp_MatchData *m, sp_int i) {SP_GC_ROOT(m);
  sp_md_check_index(m, i);
  if (!m || i < 0 || i >= m->ncap) return sp_oint_nil();
  return sp_md_char_off(m, m->caps[i * 2]);
}
sp_oint sp_MatchData_end(sp_MatchData *m, sp_int i) {SP_GC_ROOT(m);
  sp_md_check_index(m, i);
  if (!m || i < 0 || i >= m->ncap) return sp_oint_nil();
  return sp_md_char_off(m, m->caps[(i * 2) + 1]);
}
sp_IntArray *sp_MatchData_offset(sp_MatchData *m, sp_int i) {SP_GC_ROOT(m);
  sp_md_check_index(m, i);
  sp_IntArray *a = sp_IntArray_new();
  if (!m || i < 0 || i >= m->ncap) { sp_IntArray_push_nil(a); sp_IntArray_push_nil(a); return a; }
  sp_IntArray_push_o(a, sp_md_char_off(m, m->caps[i * 2]));
  sp_IntArray_push_o(a, sp_md_char_off(m, m->caps[(i * 2) + 1]));
  return a;
}
/* byte-offset accessors: the raw byte positions in source (no char conversion). */
sp_oint sp_MatchData_bytebegin(sp_MatchData *m, sp_int i) {
  sp_md_check_index(m, i);
  if (!m || i < 0 || i >= m->ncap || m->caps[i * 2] < 0) return sp_oint_nil();
  return sp_oint_of(m->caps[i * 2]);
}
sp_oint sp_MatchData_byteend(sp_MatchData *m, sp_int i) {
  sp_md_check_index(m, i);
  if (!m || i < 0 || i >= m->ncap || m->caps[i * 2] < 0) return sp_oint_nil();
  return sp_oint_of(m->caps[(i * 2) + 1]);
}
sp_IntArray *sp_MatchData_byteoffset(sp_MatchData *m, sp_int i) {SP_GC_ROOT(m);
  sp_IntArray *a = sp_IntArray_new();
  if (!m || i < 0 || i >= m->ncap || m->caps[i * 2] < 0) { sp_IntArray_push_nil(a); sp_IntArray_push_nil(a); return a; }
  sp_IntArray_push(a, m->caps[i * 2]);
  sp_IntArray_push(a, m->caps[(i * 2) + 1]);
  return a;
}
/* begin/end/offset/byte* by capture NAME (`md.begin("a")` / `:a`): resolve the
   name to its group index like MatchData#[], then defer to the index form. An
   unknown name raises IndexError, matching MRI. */
static int sp_md_group_by_name(sp_MatchData *m, const char *name) {SP_GC_ROOT_STR(name);
  int g = m ? re_named_group(m->pat, name) : -1;
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %s", name));
  return g;
}
sp_oint sp_MatchData_begin_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_begin(m, sp_md_group_by_name(m, name)); }
sp_oint sp_MatchData_end_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_end(m, sp_md_group_by_name(m, name)); }
sp_IntArray *sp_MatchData_offset_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_offset(m, sp_md_group_by_name(m, name)); }
sp_oint sp_MatchData_bytebegin_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_bytebegin(m, sp_md_group_by_name(m, name)); }
sp_oint sp_MatchData_byteend_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_byteend(m, sp_md_group_by_name(m, name)); }
sp_IntArray *sp_MatchData_byteoffset_name(sp_MatchData *m, const char *name) {SP_GC_ROOT(m);SP_GC_ROOT_STR(name); return sp_MatchData_byteoffset(m, sp_md_group_by_name(m, name)); }
/* whole-match string (group 0) -- also MatchData#to_s */
const char *sp_MatchData_to_s(sp_MatchData *m) {SP_GC_ROOT(m); const char *r = sp_MatchData_aref(m, 0); return r ? r : sp_str_empty; }
static sp_PolyArray *sp_md_groups_from(sp_MatchData *m, sp_int from) {SP_GC_ROOT(m);
  sp_PolyArray *r = sp_PolyArray_new();
  if (!m) return r;
  SP_GC_ROOT(r);
  for (sp_int i = from; i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(r, g ? sp_box_str(g) : sp_box_nil());
  }
  return r;
}
/* captures: groups 1..n-1 as a poly array (nil for non-participating) */
sp_PolyArray *sp_MatchData_captures(sp_MatchData *m) { return sp_md_groups_from(m, 1); }
/* to_a: group 0 + captures */
sp_PolyArray *sp_MatchData_to_a(sp_MatchData *m) { return sp_md_groups_from(m, 0); }
const char *sp_MatchData_pre_match(sp_MatchData *m) {SP_GC_ROOT(m);
  if (!m) return sp_str_empty;
  int e = m->caps[0];
  if (e <= 0) return sp_str_empty;
  char *b = sp_str_alloc((size_t)e);
  memcpy(b, m->source, e); b[e] = 0;
  sp_str_set_len(b, (size_t)e);
  return b;
}
const char *sp_MatchData_post_match(sp_MatchData *m) {SP_GC_ROOT(m);
  if (!m) return sp_str_empty;
  int s = m->caps[1];
  size_t sl = sp_str_byte_len(m->source);
  if (s < 0 || (size_t)s >= sl) return sp_str_empty;
  size_t len = sl - (size_t)s;
  char *b = sp_str_alloc(len);
  memcpy(b, m->source + s, len); b[len] = 0;
  sp_str_set_len(b, len);
  return b;
}
void sp_re_default_error_handler(const char *msg) {SP_GC_ROOT_STR(msg);
  /* msg points at the regex compiler's stack buffer. sp_raise_cls stores
     the pointer and longjmps past that frame, leaving it dangling -- copy
     to a GC-managed string first (mirrors sp_re_startup_error_handler).
     gcc happened to leave the stack intact; clang reused it, so e.message
     read garbage (regexp_error_catchable). */
  if (msg) {
    size_t n = strlen(msg);
    char *buf = sp_str_alloc_raw(n + 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    sp_str_set_len(buf, (size_t)n);
    msg = buf;
  }
  sp_raise_cls("RegexpError", msg);
}

/* Regexp#hash: the source AND the flags. /ab/ and /ab/i are not eql?, so a
   hash over the source alone made them collide as Hash keys (#3816). */
sp_int sp_re_hash(void *pat) {
  sp_int h = (sp_int)sp_str_hash(sp_re_source(pat));
  return h ^ ((sp_int)sp_re_options(pat) * 0x9E3779B1);
}
