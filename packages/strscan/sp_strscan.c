/*
 * sp_strscan.c -- StringScanner for Spinel
 *
 * Implements the StringScanner methods: scan, check, scan_until,
 * matched, matched?, matched_size, captures, named_captures, values_at,
 * [], pos, charpos, eos?, getch, peek, unscan, rest, terminate,
 * pre_match, post_match, string. Uses spinel's internal regex engine
 * (re_compile / spinel_re_exec -- see lib/regexp/) rather than oniguruma;
 * the existing lib/strscan.c is an oniguruma-based draft the rest of the
 * build doesn't link.
 *
 * Matching semantics: scan / check are anchored at the current
 * position (spinel_re_exec is run with start=pos and the result is
 * accepted only when the attempt that matched began at pos; the
 * match itself begins there too unless the pattern has a \K).
 * scan_until returns text from pos through the end of the next
 * match anywhere ahead.
 *
 * The scanner struct is GC-allocated directly via the shared
 * allocator (sp_alloc.h) so its lifetime tracks the one heap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* A carried-C spin package (Path B typed object), linked on demand when
   `require "strscan"` appears. Compiles against the stable package ABI. */
#include "spinel/runtime.h"

/* Forward decl mirrors lib/regexp/re_internal.h's and lib/sp_re.h's public
   API. Compiled patterns (mrb_regexp_pattern *) are malloc-owned with
   program lifetime (SP_BUILTIN_REGEX is never traced by the GC). */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
extern int spinel_re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size, int binary);
extern int re_exec_attempt(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size, int binary, int *attempt);
/* Named-capture introspection (lib/regexp/re_compile.c, lib/sp_re.c) */
extern int re_num_named(const mrb_regexp_pattern *pat);
extern const char *re_named_name(const mrb_regexp_pattern *pat, int i, int *group_out);
extern int sp_re_name_group(const mrb_regexp_pattern *pat, const char *name, const int *caps, int ncaps);
/* ArgumentError for a subject that is not valid UTF-8, as CRuby's scanner
   raises before a regexp search (lib/sp_re.c) */
extern void sp_re_check_subject(const char *str);

/* The scanner struct lives in spinel's GC heap. `source` is a GC-tracked
   string; the scan function below marks it so it survives collection. The
   last match is kept as an offset into it and a length, as CRuby's
   StringScanner keeps it, and #matched reads it out as a new String: the
   String a scan answers is the program's alone, held by no scanner. */
/* Capture slots stored from the last successful scan/check, so
   StringScanner#[] / #captures / #named_captures / #values_at can return
   numbered and named groups. caps holds [start,end) byte offsets into
   `source`, group 0 first; unmatched groups are -1. ncaps is the number
   of valid ints (2 per group, group 0 included). `pat` is the regexp
   pattern of the last successful regexp match (NULL for string-literal
   matches, getch/get_byte, dup/clone, or no match). */
#define SP_SS_MAXCAP 64
typedef struct sp_StringScanner_s {
  sp_int                    cls_id;       /* object header: runtime class id, compiler-stamped */
  const char               *source;
  const mrb_regexp_pattern *pat;          /* last matched regexp (program-lifetime, not GC-traced) */
  int64_t                   m_off, m_len; /* the last match: source[m_off, m_len] */
  int64_t                   pos;
  int64_t                   last_pos;     /* pos before the last advance (unscan rewinds here) */
  int64_t                   match_start;  /* where the last match begins (pre/post_match) --
                                             differs from last_pos after scan_until */
  int64_t                   charpos_byte; /* lazy UTF-8 charpos checkpoint: byte offset */
  int64_t                   charpos_char; /* lazy UTF-8 charpos checkpoint: char offset */
  int                       matched_p;    /* int (not sp_bool) -- keep layout compact; 0/1 */
  int                       ncaps;
  int                       caps[SP_SS_MAXCAP];
} sp_StringScanner;

static void sp_StringScanner_scan_gc(void *p) {
  sp_StringScanner *sc = (sp_StringScanner *)p;
  if (sc->source) sp_mark_string(sc->source);
}

static void sc_clear_match(sp_StringScanner *sc) {
  sc->m_len = 0;
  sc->matched_p = 0;
  sc->pat = NULL;
}

/* Record a single-group match (String pattern, getch, get_byte) matching
   CRuby's set_registers: preserve an existing larger region width with -1
   slots (so #captures matches CRuby's OnigRegion behavior) while ensuring
   at least group 0 is present. */
static void sc_set_single_cap(sp_StringScanner *sc, int64_t off, int64_t len) {
  if (sc->ncaps < 2) sc->ncaps = 2;
  sc->caps[0] = (int)off;
  sc->caps[1] = (int)(off + len);
  for (int i = 2; i < sc->ncaps; i++) sc->caps[i] = -1;
}

static void sc_reset_charpos(sp_StringScanner *sc) {
  sc->charpos_byte = 0;
  sc->charpos_char = 0;
}

sp_StringScanner *sp_StringScanner_new(sp_int cls_id, const char *str) {SP_GC_ROOT_STR(str);
  sp_StringScanner *sc = (sp_StringScanner *)sp_gc_alloc(sizeof(sp_StringScanner), NULL, sp_StringScanner_scan_gc);
  sc->cls_id = cls_id;
  sc->source = str ? str : sp_str_empty;
  sc->pat = NULL;
  sc->m_off = 0;
  sc->m_len = 0;
  sc->pos = 0;
  sc->last_pos = 0;
  sc->match_start = 0;
  sc_reset_charpos(sc);
  sc->matched_p = 0;
  sc->ncaps = 0;
  return sc;
}

/* StringScanner#dup / #clone: an independent scanner sharing the source
   string (CRuby shares the string too; scan state is copied, while
   strscan_init_copy drops the matched regexp reference). */
sp_StringScanner *sp_StringScanner_dup(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  sp_StringScanner *d = (sp_StringScanner *)sp_gc_alloc(sizeof(sp_StringScanner), NULL, sp_StringScanner_scan_gc);
  *d = *sc;
  d->pat = NULL;
  return d;
}

/* Clamp spinel_re_exec's returned ncap to the stored caps[] size. */
static int sc_clamp_ncaps(int n) {
  if (n < 0) return 0;
  if (n > SP_SS_MAXCAP) return SP_SS_MAXCAP;
  return n;
}

/* Both matchers run the pattern against the REST of the string (source + pos)
   so `^` and `\A` re-anchor at the scan pointer, as in CRuby's strscan (the
   scan pointer is the beginning of the subject). Stored capture offsets are
   shifted back to absolute source offsets. */
static void sc_shift_caps(int *caps, int ncap, int64_t pos) {
  for (int i = 0; i < ncap; i++)
    if (caps[i] >= 0) caps[i] += (int)pos;
}

/* A String pattern (`lit`, with `pat` NULL) matches its bytes literally, as
   CRuby's strscan does: the one group, the match itself. */
static int sc_lit_at(const char *lit, const char *str, int64_t slen, int64_t at, int *caps) {
  int64_t llen = (int64_t)sp_str_byte_len(lit);
  if (at + llen > slen || memcmp(str + at, lit, (size_t)llen) != 0) return 0;
  caps[0] = (int)at;
  caps[1] = (int)(at + llen);
  return 2;
}

/* Anchored match: succeed only when the match starts exactly at the scan
   pointer. Returns the length from the pointer to the match's end, or -1.
   A \K starts the match itself (caps[0], what `matched` reads) past the
   pointer; the text scan answers and steps over still starts at it. */
static int64_t sc_match_at_pos(const mrb_regexp_pattern *pat, const char *lit, const char *str, int64_t slen, int64_t pos, int *caps, int *ncap_out) {
  if (!pat) {
    if (!sc_lit_at(lit, str, slen, pos, caps)) return -1;
    if (*ncap_out < 2) *ncap_out = 2;
    for (int i = 2; i < *ncap_out; i++) caps[i] = -1;
    return caps[1] - pos;
  }
  sp_re_check_subject(str);
  int at;
  int n = re_exec_attempt(pat, str + pos, slen - pos, 0, caps, SP_SS_MAXCAP, sp_str_is_binary(str), &at);
  *ncap_out = sc_clamp_ncaps(n);
  if (n <= 0 || at != 0) { *ncap_out = 0; return -1; }
  sc_shift_caps(caps, *ncap_out, pos);
  return caps[1] - pos;
}

/* Forward-search match: returns the absolute offset of the first match at or
   after pos, and writes matched length to *mlen. -1 on miss. */
static int64_t sc_match_forward(const mrb_regexp_pattern *pat, const char *lit, const char *str, int64_t slen, int64_t pos, int64_t *mlen, int *caps, int *ncap_out) {
  if (!pat) {
    for (int64_t at = pos; at <= slen; at++) {
      if (sc_lit_at(lit, str, slen, at, caps)) {
        if (*ncap_out < 2) *ncap_out = 2;
        for (int i = 2; i < *ncap_out; i++) caps[i] = -1;
        *mlen = caps[1] - caps[0];
        return at;
      }
    }
    return -1;
  }
  sp_re_check_subject(str);
  int n = spinel_re_exec(pat, str + pos, slen - pos, 0, caps, SP_SS_MAXCAP, sp_str_is_binary(str));
  *ncap_out = sc_clamp_ncaps(n);
  if (n <= 0 || caps[0] < 0) { *ncap_out = 0; return -1; }
  sc_shift_caps(caps, *ncap_out, pos);
  *mlen = caps[1] - caps[0];
  return caps[0];
}

static char *sc_substr(const char *src, int64_t start, int64_t len) {SP_GC_ROOT_STR(src);
  char *out = sp_str_alloc((size_t)len);
  memcpy(out, src + start, (size_t)len);
  out[len] = 0;
  sp_str_set_len(out, (size_t)len);
  if (sp_str_is_binary(src)) sp_str_mark_binary(out);
  return out;
}

/* The last match, source[off, len] (an offset and a length, no String). */
static void sc_set_match(sp_StringScanner *sc, int64_t off, int64_t len) {
  sc->m_off = off;
  sc->m_len = len;
}

/* Expected byte length of a multi-byte UTF-8 lead byte (2..4), or 0. */
static int sc_utf8_lead_width(unsigned c) {
  if (c >= 0xC2 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) return 3;
  if (c >= 0xF0 && c <= 0xF4) return 4;
  return 0;
}

/* Whether the second byte of a 3- or 4-byte sequence violates RFC 3629
   (overlong E0, surrogate half ED, or > U+10FFFF F4). */
static int sc_utf8_bad_second(unsigned c, int n, unsigned c1) {
  if (n == 3 && ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0))) return 1;
  if (n == 4 && ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))) return 1;
  return 0;
}

/* Valid RFC 3629 multi-byte UTF-8 sequence length at p (2..4), or 0 when
   not a valid multi-byte character. Matches sp_utf8_valid_len in lib/sp_str.h
   (kept local because packages include only spinel/runtime.h). */
static int sc_utf8_valid_len(const unsigned char *p, size_t avail) {
  int n = sc_utf8_lead_width(p[0]);
  if (!n || avail < (size_t)n) return 0;
  for (int i = 1; i < n; i++)
    if ((p[i] & 0xC0) != 0x80) return 0;
  if (sc_utf8_bad_second(p[0], n, p[1])) return 0;
  return n;
}

/* Whether the remaining `avail` bytes at p are a valid prefix of a multi-byte
   UTF-8 sequence cut short by the current scan pointer (e.g. pos= landed
   mid-character). The charpos checkpoint stops before such a prefix so a
   subsequent advance past the character counts it as 1 instead of N bytes. */
static int sc_utf8_truncated_prefix(const unsigned char *p, size_t avail) {
  int n = sc_utf8_lead_width(p[0]);
  if (!n || avail >= (size_t)n) return 0;
  for (size_t i = 1; i < avail; i++)
    if ((p[i] & 0xC0) != 0x80) return 0;
  if (avail >= 2 && sc_utf8_bad_second(p[0], n, p[1])) return 0;
  return 1;
}

/* Byte length of the character starting at src[pos], respecting binary
   encoding and RFC 3629 UTF-8 validity (malformed bytes take 1 byte). */
static int64_t sc_char_len(const char *src, int64_t pos, int64_t len) {
  if (sp_str_is_binary(src)) return 1;
  const unsigned char *p = (const unsigned char *)src + pos;
  if (*p < 0x80) return 1;
  int w = sc_utf8_valid_len(p, (size_t)(len - pos));
  return w > 0 ? (int64_t)w : 1;
}

static const char *sc_scan(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, lit, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc_clear_match(sc);
    return NULL;
  }
  char *m = sc_substr(sc->source, sc->pos, mlen);
  SP_GC_ROOT_STR(m);
  sc->last_pos = sc->pos;
  sc->match_start = sc->caps[0];
  sc_set_match(sc, sc->caps[0], sc->caps[1] - sc->caps[0]);
  sc->matched_p = 1;
  sc->pat = pat;
  sc->pos += mlen;
  return m;
}

static const char *sc_check(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, lit, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc_clear_match(sc);
    return NULL;
  }
  char *m = sc_substr(sc->source, sc->pos, mlen);
  SP_GC_ROOT_STR(m);
  sc->last_pos = sc->pos;
  sc->match_start = sc->caps[0];
  sc_set_match(sc, sc->caps[0], sc->caps[1] - sc->caps[0]);
  sc->matched_p = 1;
  sc->pat = pat;
  return m;
}

static const char *sc_scan_until(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = 0;
  int64_t mstart = sc_match_forward(pat, lit, sc->source, slen, sc->pos, &mlen, sc->caps, &sc->ncaps);
  if (mstart < 0) {
    sc_clear_match(sc);
    return NULL;
  }
  int64_t consumed = (mstart + mlen) - sc->pos;
  char *taken = sc_substr(sc->source, sc->pos, consumed);
  SP_GC_ROOT_STR(taken);
  sc->last_pos = sc->pos;
  sc->match_start = mstart;  /* pre_match is everything before the MATCH */
  sc_set_match(sc, mstart, mlen);
  sc->matched_p = 1;
  sc->pat = pat;
  sc->pos = mstart + mlen;
  return taken;
}

/* StringScanner#check_until: scan_until without advancing the pointer. */
static const char *sc_check_until(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = 0;
  int64_t mstart = sc_match_forward(pat, lit, sc->source, slen, sc->pos, &mlen, sc->caps, &sc->ncaps);
  if (mstart < 0) {
    sc_clear_match(sc);
    return NULL;
  }
  char *taken = sc_substr(sc->source, sc->pos, (mstart + mlen) - sc->pos);
  SP_GC_ROOT_STR(taken);
  sc->last_pos = sc->pos;
  sc->match_start = mstart;
  sc_set_match(sc, mstart, mlen);
  sc->matched_p = 1;
  sc->pat = pat;
  return taken;
}

/* StringScanner#match?: anchored like scan, records match state, returns the
   matched byte length without advancing (nil on miss). */
static sp_RbVal sc_match_q(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, lit, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc_clear_match(sc);
    return sp_box_nil();
  }
  sc->last_pos = sc->pos;
  sc->match_start = sc->caps[0];
  sc_set_match(sc, sc->caps[0], sc->caps[1] - sc->caps[0]);
  sc->matched_p = 1;
  sc->pat = pat;
  return sp_box_int((sp_int)mlen);
}

/* StringScanner#skip: anchored like scan, advances, returns the matched byte
   length (nil on miss). */
static sp_RbVal sc_skip(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, lit, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc_clear_match(sc);
    return sp_box_nil();
  }
  sc->last_pos = sc->pos;
  sc->match_start = sc->caps[0];
  sc_set_match(sc, sc->caps[0], sc->caps[1] - sc->caps[0]);
  sc->matched_p = 1;
  sc->pat = pat;
  sc->pos += mlen;
  return sp_box_int((sp_int)mlen);
}

/* StringScanner#exist?: forward search without advancing; returns the byte
   distance from the pointer to the END of the match (nil on miss). */
static sp_RbVal sc_exist_q(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = 0;
  int64_t mstart = sc_match_forward(pat, lit, sc->source, slen, sc->pos, &mlen, sc->caps, &sc->ncaps);
  if (mstart < 0) {
    sc_clear_match(sc);
    return sp_box_nil();
  }
  sc->last_pos = sc->pos;
  sc->match_start = mstart;
  sc_set_match(sc, mstart, mlen);
  sc->matched_p = 1;
  sc->pat = pat;
  return sp_box_int((sp_int)((mstart + mlen) - sc->pos));
}

/* StringScanner#skip_until: forward search, advances past the match, returns
   the consumed byte count (nil on miss). */
static sp_RbVal sc_skip_until(sp_StringScanner *sc, const mrb_regexp_pattern *pat, const char *lit) {SP_GC_ROOT(sc);
  if (!sc || (!pat && !lit)) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = 0;
  int64_t mstart = sc_match_forward(pat, lit, sc->source, slen, sc->pos, &mlen, sc->caps, &sc->ncaps);
  if (mstart < 0) {
    sc_clear_match(sc);
    return sp_box_nil();
  }
  int64_t consumed = (mstart + mlen) - sc->pos;
  sc->last_pos = sc->pos;
  sc->match_start = mstart;
  sc_set_match(sc, mstart, mlen);
  sc->matched_p = 1;
  sc->pat = pat;
  sc->pos = mstart + mlen;
  return sp_box_int((sp_int)consumed);
}

/* The bindings: a Regexp pattern, or a String one matched literally. */
const char *sp_StringScanner_scan(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_scan(sc, pat, NULL); }
const char *sp_StringScanner_scan_str(sp_StringScanner *sc, const char *lit) { return sc_scan(sc, NULL, lit); }
const char *sp_StringScanner_check(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_check(sc, pat, NULL); }
const char *sp_StringScanner_check_str(sp_StringScanner *sc, const char *lit) { return sc_check(sc, NULL, lit); }
const char *sp_StringScanner_scan_until(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_scan_until(sc, pat, NULL); }
const char *sp_StringScanner_scan_until_str(sp_StringScanner *sc, const char *lit) { return sc_scan_until(sc, NULL, lit); }
const char *sp_StringScanner_check_until(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_check_until(sc, pat, NULL); }
const char *sp_StringScanner_check_until_str(sp_StringScanner *sc, const char *lit) { return sc_check_until(sc, NULL, lit); }
sp_RbVal sp_StringScanner_match_q(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_match_q(sc, pat, NULL); }
sp_RbVal sp_StringScanner_match_q_str(sp_StringScanner *sc, const char *lit) { return sc_match_q(sc, NULL, lit); }
sp_RbVal sp_StringScanner_skip(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_skip(sc, pat, NULL); }
sp_RbVal sp_StringScanner_skip_str(sp_StringScanner *sc, const char *lit) { return sc_skip(sc, NULL, lit); }
sp_RbVal sp_StringScanner_exist_q(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_exist_q(sc, pat, NULL); }
sp_RbVal sp_StringScanner_exist_q_str(sp_StringScanner *sc, const char *lit) { return sc_exist_q(sc, NULL, lit); }
sp_RbVal sp_StringScanner_skip_until(sp_StringScanner *sc, mrb_regexp_pattern *pat) { return sc_skip_until(sc, pat, NULL); }
sp_RbVal sp_StringScanner_skip_until_str(sp_StringScanner *sc, const char *lit) { return sc_skip_until(sc, NULL, lit); }

/* StringScanner#[n] -- the nth capture group of the last scan/check.
   Group 0 is the whole match; a negative index counts back from the last
   group. Returns NULL (Ruby nil) when there was no match, the index is out
   of range, or the group didn't participate. */
const char *sp_StringScanner_aref(sp_StringScanner *sc, sp_int n) {SP_GC_ROOT(sc);
  if (!sc || !sc->matched_p) return NULL;
  int ngroups = sc->ncaps / 2;
  int idx = (int)n;
  if (idx < 0) idx += ngroups;
  if (idx < 0 || idx >= ngroups) return NULL;
  int st = sc->caps[2 * idx];
  int en = sc->caps[2 * idx + 1];
  if (st < 0 || en < st) return NULL;
  return sc_substr(sc->source, st, en - st);
}

/* Shared named-capture lookup for both String and Symbol keys. Callers root
   `sc` and any GC-managed `name` before calling. */
static const char *sc_aref_name_bytes(sp_StringScanner *sc, const char *name, size_t nlen) {
  if (!sc || !sc->matched_p) return NULL;
  int g = (sc->pat && memchr(name, 0, nlen) == NULL)
            ? sp_re_name_group(sc->pat, name, sc->caps, sc->ncaps)
            : -1;
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %.*s", (int)nlen, name));
  return sp_StringScanner_aref(sc, g);
}

/* StringScanner#["name"] -- named capture lookup. Returns NULL when there is
   no active match; raises IndexError when the group name is undefined (or
   when the last match had no regexp, e.g. a String pattern or getch). */
const char *sp_StringScanner_aref_name(sp_StringScanner *sc, const char *name) {SP_GC_ROOT(sc); SP_GC_ROOT_STR(name);
  if (!sc || !sc->matched_p) return NULL;
  if (!name) name = sp_str_empty;
  return sc_aref_name_bytes(sc, name, sp_str_byte_len(name));
}

/* StringScanner#[idx] for Symbol or dynamic/boxed arguments (Integer, Symbol,
   String, Float, or object responding to #to_int). When not matched, CRuby
   returns nil immediately before checking the argument type. */
const char *sp_StringScanner_aref_any(sp_StringScanner *sc, sp_RbVal idx) {SP_GC_ROOT(sc); SP_GC_ROOT_RBVAL(idx);
  if (!sc || !sc->matched_p) return NULL;
  if (idx.tag == SP_TAG_INT) return sp_StringScanner_aref(sc, idx.v.i);
  if (idx.tag == SP_TAG_SYM) {
    const char *nm = sp_sym_name_fn ? sp_sym_name_fn((sp_sym)idx.v.i) : sp_str_empty;
    return sc_aref_name_bytes(sc, nm, sp_str_byte_len(nm));
  }
  if (sp_rbval_is_string(idx)) {
    const char *nm = sp_rbval_string_bytes(idx);
    return sc_aref_name_bytes(sc, nm, sp_str_byte_len(nm));
  }
  return sp_StringScanner_aref(sc, sp_poly_arg_int(idx));
}

const char *sp_StringScanner_matched(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  return sc_substr(sc->source, sc->m_off, sc->m_len);
}

sp_bool sp_StringScanner_matched_p(sp_StringScanner *sc) {
  if (!sc) return FALSE;
  return sc->matched_p ? TRUE : FALSE;
}

/* StringScanner#matched_size: byte length of the last match, or nil. */
sp_RbVal sp_StringScanner_matched_size(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return sp_box_nil();
  return sp_box_int((sp_int)sc->m_len);
}

/* StringScanner#captures: Array of capture groups 1..ncap-1 (nil for
   non-participating groups), or nil when not matched. */
sp_RbVal sp_StringScanner_captures(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sc || !sc->matched_p) return sp_box_nil();
  int ngroups = sc->ncaps / 2;
  sp_PolyArray *a = sp_PolyArray_new_cap(ngroups > 1 ? ngroups - 1 : 0);
  SP_GC_ROOT(a);
  for (int i = 1; i < ngroups; i++) {
    const char *g = sp_StringScanner_aref(sc, i);
    sp_RbVal v = g ? sp_box_str_new(g) : sp_box_nil();
    SP_GC_ROOT_RBVAL(v);
    sp_PolyArray_push(a, v);
  }
  return sp_box_poly_array(a);
}

/* StringScanner#named_captures: {String name => String | nil}. Returns an
   empty Hash when not matched or when the matched pattern has no named
   groups (matches CRuby). */
sp_RbVal sp_StringScanner_named_captures(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sp_json_mk_hash_fn) return sp_box_nil();
  sp_RbVal h = sp_json_mk_hash_fn();
  SP_GC_ROOT_RBVAL(h);
  if (!sc || !sc->matched_p || !sc->pat || !sp_json_hash_set_fn) return h;
  int n = re_num_named(sc->pat);
  for (int i = 0; i < n; i++) {
    int g = -1;
    const char *nm = re_named_name(sc->pat, i, &g);
    if (!nm) continue;
    const char *k = sp_hash_key_from_bytes(nm, strlen(nm), 0);
    SP_GC_ROOT_STR(k);
    g = sp_re_name_group(sc->pat, nm, sc->caps, sc->ncaps);
    const char *val = sp_StringScanner_aref(sc, g);
    sp_RbVal v = val ? sp_box_str_new(val) : sp_box_nil();
    SP_GC_ROOT_RBVAL(v);
    sp_json_hash_set_fn(h, k, v);
  }
  return h;
}

/* StringScanner#values_at(*indices): Array of captures for each index
   (Integer, String, Symbol, or #to_int), or nil when not matched. */
sp_RbVal sp_StringScanner_values_at(sp_StringScanner *sc, sp_int argc, sp_RbVal *argv) {SP_GC_ROOT(sc);
  if (!sc || !sc->matched_p) return sp_box_nil();
  SP_GC_SAVE();
  for (sp_int i = 0; i < argc; i++) _sp_gc_root_push((void **)((uintptr_t)&argv[i] | (uintptr_t)1));
  sp_PolyArray *a = sp_PolyArray_new_cap(argc > 0 ? argc : 0);
  SP_GC_ROOT(a);
  for (sp_int i = 0; i < argc; i++) {
    const char *g = sp_StringScanner_aref_any(sc, argv[i]);
    sp_RbVal v = g ? sp_box_str_new(g) : sp_box_nil();
    SP_GC_ROOT_RBVAL(v);
    sp_PolyArray_push(a, v);
  }
  return sp_box_poly_array(a);
}

sp_int sp_StringScanner_pos(sp_StringScanner *sc) {
  if (!sc) return 0;
  return (sp_int)sc->pos;
}

/* StringScanner#charpos: character offset of the scan pointer.
   O(1) for binary and ASCII-7 strings; lazy O(1)-amortized checkpoint
   cursor over UTF-8 strings (counting RFC 3629 sequences and malformed
   bytes individually, matching CRuby's rb_enc_strlen on source[0, pos)). */
sp_int sp_StringScanner_charpos(sp_StringScanner *sc) {
  if (!sc || sc->pos <= 0) return 0;
  size_t fb;
  if (sp_str_fixed_width(sc->source, &fb) || sp_str_is_binary(sc->source))
    return (sp_int)sc->pos;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t pos = sc->pos > slen ? slen : sc->pos;
  if (pos < sc->charpos_byte) sc_reset_charpos(sc);
  if (pos == sc->charpos_byte) return (sp_int)sc->charpos_char;
  const unsigned char *base = (const unsigned char *)sc->source;
  const unsigned char *p = base + sc->charpos_byte;
  const unsigned char *end = base + pos;
  int64_t n = sc->charpos_char;
  for (;;) {
    while ((size_t)(end - p) >= sizeof(uint64_t)) {
      uint64_t w;
      memcpy(&w, p, sizeof(w));
      if (w & 0x8080808080808080ULL) break;
      p += sizeof(w);
      n += (int64_t)sizeof(w);
    }
    if (p == end) break;
    if (*p < 0x80) {
      p++;
      n++;
    }
    else {
      size_t avail = (size_t)(end - p);
      int width = sc_utf8_valid_len(p, avail);
      if (width > 0) {
        p += width;
        n++;
      }
      else if (sc_utf8_truncated_prefix(p, avail)) {
        sc->charpos_byte = (int64_t)(p - base);
        sc->charpos_char = n;
        return (sp_int)(n + (int64_t)avail);
      }
      else {
        p++;
        n++;
      }
    }
  }
  sc->charpos_byte = pos;
  sc->charpos_char = n;
  return (sp_int)n;
}

sp_int sp_StringScanner_pos_set(sp_StringScanner *sc, sp_int p) {SP_GC_ROOT(sc);
  if (!sc) return 0;
  /* CRuby: a negative position counts from the end; anything out of the
     [0, bytesize] range raises RangeError (never index out of the buffer). */
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t np = (int64_t)p;
  if (np < 0) np += slen;
  if (np < 0 || np > slen) sp_raise_cls("RangeError", "index out of range");
  sc->pos = np;
  return p;
}

sp_bool sp_StringScanner_eos_p(sp_StringScanner *sc) {
  if (!sc) return TRUE;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  return (sc->pos >= slen) ? TRUE : FALSE;
}

const char *sp_StringScanner_getch(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  if (sc->pos >= slen) {
    sc_clear_match(sc);
    return NULL;
  }
  int64_t clen = sc_char_len(sc->source, sc->pos, slen);
  char *c = sc_substr(sc->source, sc->pos, clen);
  sc->last_pos = sc->pos;
  sc->match_start = sc->pos;
  sc_set_match(sc, sc->pos, clen);
  sc_set_single_cap(sc, sc->pos, clen);
  sc->pat = NULL;
  sc->matched_p = 1;
  sc->pos += clen;
  return c;
}

/* #get_byte / #scan_byte: the byte at the pointer, taken (a one-byte
   String, or its value), and #matched; nil at the end. #peek_byte reads it
   without moving or matching. */
const char *sp_StringScanner_get_byte(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sc) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  if (sc->pos >= slen) {
    sc_clear_match(sc);
    return NULL;
  }
  char *c = sc_substr(sc->source, sc->pos, 1);
  sc->last_pos = sc->pos;
  sc->match_start = sc->pos;
  sc_set_match(sc, sc->pos, 1);
  sc_set_single_cap(sc, sc->pos, 1);
  sc->pat = NULL;
  sc->matched_p = 1;
  sc->pos += 1;
  return c;
}
sp_RbVal sp_StringScanner_scan_byte(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sc) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  if (sc->pos >= slen) {
    sc_clear_match(sc);
    return sp_box_nil();
  }
  sp_int b = (unsigned char)sc->source[sc->pos];
  sp_StringScanner_get_byte(sc);
  return sp_box_int(b);
}
sp_RbVal sp_StringScanner_peek_byte(sp_StringScanner *sc) {
  if (!sc) return sp_box_nil();
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  if (sc->pos >= slen) return sp_box_nil();
  return sp_box_int((unsigned char)sc->source[sc->pos]);
}

const char *sp_StringScanner_peek(sp_StringScanner *sc, sp_int n) {
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t avail = slen - sc->pos;
  int64_t take = n < avail ? n : avail;
  if (take < 0) take = 0;
  return sc_substr(sc->source, sc->pos, take);
}

sp_StringScanner *sp_StringScanner_unscan(sp_StringScanner *sc) {SP_GC_ROOT(sc);
  if (!sc) return sc;
  /* CRuby raises StringScanner::Error when there is no match record to
     rewind to: a double unscan, or unscan before any successful scan.
     Guard before mutating state so a failed unscan leaves pos intact. */
  if (!sc->matched_p) {
    sp_raise_cls("StringScanner::Error",
                 "unscan failed: previous match record not exist");
  }
  sc->pos = sc->last_pos;
  sc_clear_match(sc);
  sc_reset_charpos(sc);
  return sc;
}

const char *sp_StringScanner_rest(sp_StringScanner *sc) {
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - sc->pos;
  if (rem <= 0) return sc_substr(sc->source, 0, 0);   /* a new String, as each answer is */
  return sc_substr(sc->source, sc->pos, rem);
}

sp_int sp_StringScanner_rest_size(sp_StringScanner *sc) {
  if (!sc) return 0;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - sc->pos;
  return rem < 0 ? 0 : (sp_int)rem;
}

sp_bool sp_StringScanner_rest_p(sp_StringScanner *sc) {
  if (!sc) return FALSE;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  return (sc->pos < slen) ? TRUE : FALSE;
}

sp_StringScanner *sp_StringScanner_terminate(sp_StringScanner *sc) {
  if (!sc) return sc;
  sc->pos = (int64_t)sp_str_byte_len(sc->source);
  sc_clear_match(sc);
  return sc;
}

const char *sp_StringScanner_string(sp_StringScanner *sc) {
  if (!sc) return sp_str_empty;
  return sc->source ? sc->source : sp_str_empty;
}

const char *sp_StringScanner_pre_match(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  if (sc->match_start <= 0) return sc_substr(sc->source, 0, 0);
  return sc_substr(sc->source, 0, sc->match_start);
}

const char *sp_StringScanner_post_match(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  int64_t mlen = sc->m_len;
  int64_t start = sc->match_start + mlen;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - start;
  if (rem <= 0) return sc_substr(sc->source, 0, 0);   /* a new String, as each answer is */
  return sc_substr(sc->source, start, rem);
}

/* #string=: scan another String from the start (CRuby resets the scanner
   as #reset does) and answer it */
const char *sp_StringScanner_string_set(sp_StringScanner *sc, const char *str) {
  if (!sc) return str;
  sp_gc_wb((void *)sc);
  sc->source = str ? str : sp_str_empty;
  sc->pos = 0;
  sc->last_pos = 0;
  sc->match_start = 0;
  sc_clear_match(sc);
  sc_reset_charpos(sc);
  return str;
}

sp_StringScanner *sp_StringScanner_reset(sp_StringScanner *sc) {
  if (!sc) return sc;
  sc->pos = 0;
  sc->last_pos = 0;
  sc_clear_match(sc);
  sc_reset_charpos(sc);
  return sc;
}
