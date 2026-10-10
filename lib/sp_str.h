#ifndef SP_STR_H
#define SP_STR_H
/* sp_str.h -- cold String transforms compiled once in lib/sp_str.c and
 * lib/sp_str_cold.c.
 *
 * These are leaf `const char*` operations (case, strip, split-family,
 * partition, dump/undump, concat, repeat, ...) that depend only on the
 * shared string heap (sp_alloc.h) and the typed arrays (sp_array.h). The
 * The UTF-8 decode/advance/encode + length-cache lookup are inline here
 * (relocated from spinel_rt.h, codegen-neutral). The FNV hash cascade
 * (#282) and sp_str_eq stay inline in spinel_rt.h (optcarrot-sensitive).
 *
 * sp_sprintf / sp_raise_cls are provided by the generated TU and resolved
 * at the final link (same as lib/sp_core.c). */
#include "sp_array.h"   /* sp_StrArray, sp_IntArray + sp_alloc.h / sp_gc.h / sp_types.h */

const char *sp_sprintf(const char *fmt, ...) SP_PRINTF_FORMAT(1, 2);  /* defined in the generated TU */

/* ---- hot UTF-8 + length-cache inline core (relocated from spinel_rt.h;
   each generated TU still inlines these identically). Length-cache state
   (sp_str_lcache / SP_STR_LCACHE_*) lives in sp_alloc.h / sp_alloc.c. ---- */
static inline int sp_utf8_char_len(unsigned char c){if(c<0x80)return 1;if(c<0xC0)return 1;if(c<0xE0)return 2;if(c<0xF0)return 3;return 4;}
static inline int sp_utf8_advance(const char*p){int cn=sp_utf8_char_len((unsigned char)*p);int i=1;while(i<cn&&((unsigned char)p[i]&0xC0)==0x80)i++;return i;}
static inline int sp_utf8_decode(const char*p,uint32_t*out){unsigned char c=(unsigned char)p[0];if(c<0x80){*out=c;return 1;}if(c<0xC0){*out=c;return 1;}unsigned char c1=(unsigned char)p[1];if((c1&0xC0)!=0x80){*out=c;return 1;}if(c<0xE0){*out=((uint32_t)(c&0x1F)<<6)|(c1&0x3F);return 2;}unsigned char c2=(unsigned char)p[2];if((c2&0xC0)!=0x80){*out=c;return 1;}if(c<0xF0){*out=((uint32_t)(c&0x0F)<<12)|((uint32_t)(c1&0x3F)<<6)|(c2&0x3F);return 3;}unsigned char c3=(unsigned char)p[3];if((c3&0xC0)!=0x80){*out=c;return 1;}*out=((uint32_t)(c&0x07)<<18)|((uint32_t)(c1&0x3F)<<12)|((uint32_t)(c2&0x3F)<<6)|(c3&0x3F);return 4;}
static inline int sp_utf8_encode(uint32_t cp,char*out){if(cp<0x80){out[0]=(char)cp;return 1;}if(cp<0x800){out[0]=(char)(0xC0|(cp>>6));out[1]=(char)(0x80|(cp&0x3F));return 2;}if(cp<0x10000){out[0]=(char)(0xE0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&0x3F));out[2]=(char)(0x80|(cp&0x3F));return 3;}out[0]=(char)(0xF0|(cp>>18));out[1]=(char)(0x80|((cp>>12)&0x3F));out[2]=(char)(0x80|((cp>>6)&0x3F));out[3]=(char)(0x80|(cp&0x3F));return 4;}
#define sp_str_lcache_hash sp_str_lcache_slot
/* 0xf1 is frozen -- an explicitly frozen heap string, and every string literal
   (frozen string literals are permanent). Its contents can never change, so its
   character length is the most cacheable of all; leaving it out meant #length,
   #[] and every character index on a literal rescanned the whole string on each
   call. 0xfd (the sp_String append buffer) is in as well: its length moves, but
   every mutation goes through sp_fd_publish, which drops the entry -- without
   it a scan loop over a built-up string rewalked the whole buffer per index. */
static inline int sp_str_cacheable(const char *s) {
  unsigned char m = ((const unsigned char *)s)[-1];
  return m == 0xfe || m == 0xfc || m == 0xff || m == 0xf1 || m == 0xfd || m == 0xfb || m == 0xfa || m == 0xf8;
}
/* Byte-exact comparison of two heap strings. strcmp stops at the first NUL,
   so two strings that differ only after one compared equal and sorted equal --
   `\0` is an ordinary byte in a Ruby String, and the representation carries
   the real length (#3471, #3472). Ordering matches CRuby's: memcmp over the
   common prefix, then the shorter string first. */
static inline int sp_str_cmp_bytes(const char *a, const char *b) {
  size_t la = sp_str_byte_len(a), lb = sp_str_byte_len(b);
  size_t n = la < lb ? la : lb;
  int r = n ? memcmp(a, b, n) : 0;
  if (r) return r < 0 ? -1 : 1;
  if (la != lb) return la < lb ? -1 : 1;
  /* identical bytes: CRuby falls back to the encoding index when the two are
     not comparable, and ASCII-8BIT sorts before UTF-8 (rb_str_cmp). Both
     ASCII-only stays 0, the way sp_str_eq answers true for it. */
  { int ba = sp_str_is_binary(a), bb = sp_str_is_binary(b);
    if (ba == bb) return 0;
    for (size_t i = 0; i < la; i++)
      if ((unsigned char)a[i] >= 0x80) return ba ? -1 : 1;
    return 0; }
}
static inline void sp_str_split_push(sp_StrArray*a,const char*p,size_t n){
  char*r=sp_str_alloc_raw(n+1);
  memcpy(r,p,n);
  r[n]=0;
  /* record the real length: alloc_raw leaves it unset, so a piece holding an
     embedded NUL would answer strlen and read short */
  sp_str_set_len(r,n);
  sp_StrArray_push(a,r);
}

const char *sp_bytestr(const char *hay, size_t hn, const char *need, size_t nn);   /* byte-wise substring search; lib/sp_str.c, also used by lib/sp_str_cold.c */
int sp_utf8_set_has(const uint32_t*cps,size_t n,uint32_t cp);
uint32_t sp_uc_toupper(uint32_t cp);
uint32_t sp_uc_tolower(uint32_t cp);
sp_int sp_str_casecmp(const char*a,const char*b);
sp_bool sp_str_casecmp_p(const char*a,const char*b);
sp_bool sp_str_valid_encoding(const char*s);
const char*sp_str_field(const char*s,const char*sep,sp_int n);
sp_int sp_str_field_count(const char*s,const char*sep);
const char*sp_str_concat(const char*a,const char*b);
const char*sp_str_append_bytes(const char*a,const char*b);
const char*sp_str_concat3(const char*a,const char*b,const char*c);
const char*sp_str_concat4(const char*a,const char*b,const char*c,const char*d);
const char*sp_str_concat_arr(const char *const *parts,int n);
const char*sp_str_inspect(const char*s);
sp_bool sp_sym_plain_name_p(const char *p, sp_bool allow_suffix);
sp_bool sp_sym_simple_p(const char *n);
const char *sp_sym_inspect_name(const char *name);
const char *sp_sym_inspect_key(const char *name);
const char*sp_str_upcase(const char*s);
const char*sp_str_downcase(const char*s);
const char*sp_str_swapcase(const char*s);
const char*sp_str_upcase_ascii(const char*s);
const char*sp_str_downcase_ascii(const char*s);
const char*sp_str_swapcase_ascii(const char*s);
const char*sp_str_capitalize_ascii(const char*s);
const char*sp_str_dump(const char*s);
const char*sp_str_delete_prefix(const char*s,const char*p);
const char*sp_str_substr(const char*s,sp_int start,sp_int len);
const char *sp_str_append_grow(const char *s, const char *t);
const char *sp_str_append_grow_n(const char *s, const char *t, size_t lb);   /* the first lb bytes of t */
const char*sp_str_delete_suffix(const char*s,const char*p);
const char*sp_str_strip(const char*s);
const char*sp_str_chomp(const char*s);
const char *sp_str_chomp_sep(const char *s, const char *sep);
const char*sp_str_chop(const char*s);
const char*sp_str_chr(const char*s);
sp_int sp_str_byte_to_char(const char*s,sp_int byteoff);
sp_bool sp_str_include(const char*s,const char*sub);
sp_bool sp_str_start_with(const char*s,const char*p);
sp_bool sp_str_end_with(const char*s,const char*suf);
sp_StrArray *sp_str_partition(const char *s, const char *sep);
sp_StrArray *sp_str_rpartition(const char *s, const char *sep);
sp_StrArray*sp_str_lines(const char*s);
sp_StrArray*sp_str_lines_sep(const char*s,const char*sep);
sp_StrArray*sp_str_lines_sep_chomp(const char*s,const char*sep);
sp_StrArray*sp_str_lines_chomp(const char*s);
const char*sp_str_byteslice(const char*s,sp_int start,sp_int len);
const char*sp_str_byteslice1(const char*s,sp_int i);
const char*sp_str_byteslice_range(const char*s,sp_int lo,sp_int hi,int excl,int lo_none,int hi_none);
const char*sp_str_bytesplice(const char*s,sp_int start,sp_int len,const char*val);
int sp_str_ascii_only(const char*s);
int sp_str_enc_step(int state, const char *acc, size_t acc_len, const char *part, size_t part_len, int part_bin);
/* the common step inline: a part in the encoding so far keeps it */
static inline int sp_str_enc_step_i(int state, const char *acc, size_t acc_len, const char *part, size_t part_len, int part_bin) {
  if (state == part_bin) return state;
  return sp_str_enc_step(state, acc, acc_len, part, part_len, part_bin);
}
const char*sp_str_format_strarr(const char*fmt,sp_StrArray*a);
const char*sp_str_sub(const char*s,const char*pat,const char*rep);
const char*sp_str_remove_first(const char*s,const char*pat);
const char*sp_str_capitalize(const char*s);
const char*sp_str_repeat(const char*s,sp_int n);
sp_IntArray*sp_str_bytes(const char*s);
const char *sp_str_crypt(const char *s, const char *salt);
const char*sp_str_lstrip(const char*s);
const char*sp_str_rstrip(const char*s);
const char*sp_str_dup(const char*s);
const char *sp_plain_char(unsigned char c);
const char *sp_bin_char(unsigned char c);
const char*sp_str_b(const char*s);

/* ---- utf8-dependent cold transforms (lib/sp_str.c, lib/sp_str_cold.c) ---- */
/* nil-receiver raise: a nullable string carries nil as NULL, and CRuby
   answers NoMethodError. The bare primitives below stay total over NULL
   (runtime internals pass legitimately-nil elements); the _m/_p variants
   carry Ruby receiver semantics at method call sites. */
SP_NORETURN SP_COLD void sp_nil_recv(const char *meth);
sp_int sp_str_length_m(const char *s);
sp_int sp_str_bytesize_m(const char *s);
sp_bool sp_str_empty_p(const char *s);
const char *sp_str_plus(const char *a, const char *b);
const char *sp_str_plus_lit(const char *a, const char *lit, size_t ll);
sp_int sp_str_count_chars(const char *s, size_t bl);
sp_int sp_str_length(const char*s);
sp_int sp_str_ord(const char*s);
size_t sp_utf8_byte_offset(const char*s,sp_int char_idx);
uint32_t*sp_utf8_decode_all(const char*s,size_t*out_n);
uint32_t*sp_utf8_decode_charset(const char*s,size_t*out_n);
uint32_t*sp_utf8_decode_charset_n(const char*s,size_t bl,size_t*out_n);
void sp_str_split_into(sp_StrArray*a,const char*s,const char*sep);
const char*sp_str_undump(const char*s);
const char*sp_str_succ_impl(const char*s);
const char*sp_str_succ(const char*s);
const char*sp_str_succ_n(const char*s,sp_int n);
sp_StrArray*sp_str_split(const char*s,const char*sep);
sp_StrArray*sp_str_split_drop_trailing(const char*s,const char*sep);
sp_StrArray*sp_str_split_limit(const char*s,const char*sep,sp_int n);
sp_StrArray*sp_str_split_ws(const char*s);
sp_StrArray*sp_str_split_ws_limit(const char*s,sp_int n);
sp_StrArray*sp_str_scan(const char*s,const char*pat);
sp_int sp_str_scan_at(const char*s,const char*pat,sp_int pos);
const char*sp_str_gsub(const char*s,const char*pat,const char*rep);
sp_int sp_str_index(const char*s,const char*sub);
sp_int sp_str_index_from(const char*s,const char*sub,sp_int start);
sp_int sp_str_rindex(const char*s,const char*sub);
/* the position, nil on a miss */
sp_oint sp_str_rindex_from(const char*s,const char*sub,sp_int pos);
sp_oint sp_str_byteindex(const char*s,const char*sub);
sp_oint sp_str_byteindex_from(const char*s,const char*sub,sp_int start);
sp_oint sp_str_byterindex(const char*s,const char*sub);
sp_oint sp_str_byterindex_from(const char*s,const char*sub,sp_int pos);
const char*sp_str_sub_range(const char*s,sp_int start,sp_int len);
const char*sp_str_char_at_or_nil(const char*s,sp_int i);
const char*sp_str_sub_range_len(const char*s,sp_int cl,sp_int start,sp_int len);
const char*sp_str_sub_range_r(const char*s,sp_int start,sp_int end_,sp_int excl);
const char*sp_str_sub_range_len_r(const char*s,sp_int cl,sp_int start,sp_int end_,sp_int excl);
const char*sp_str_reverse(const char*s);
sp_int sp_str_count(const char*s,const char*chars);
sp_int sp_str_count_n(const char*s,const char**chars,sp_int n);
sp_int sp_str_sum_bits(const char*s,sp_int bits);   /* String#sum(bits=16) */
sp_IntArray*sp_str_codepoints(const char*s);
sp_IntArray*sp_str_codepoints_all(const char*s);
sp_StrArray*sp_str_chars(const char*s);
const char*sp_str_tr(const char*s,const char*from,const char*to);
const char*sp_str_tr_s(const char*s,const char*from,const char*to);
const char*sp_str_delete(const char*s,const char*chars);
const char*sp_str_squeeze(const char*s);
const char*sp_str_squeeze_chars(const char*s,const char*cs);
const char*sp_str_delete_n(const char*s,const char**chars,sp_int n);
const char*sp_str_strip_n(const char*s,const char**chars,sp_int n);
const char*sp_str_lstrip_n(const char*s,const char**chars,sp_int n);
const char*sp_str_rstrip_n(const char*s,const char**chars,sp_int n);
const char*sp_str_squeeze_n(const char*s,const char**chars,sp_int n);
const char *sp_str_scrub(const char *s, const char *repl);
const char *sp_str_scrub_utf8(const char *s, const char *repl);
sp_int sp_str_scrub_bad(const char *s, sp_int bl, sp_int from, sp_int *len);
const char *sp_str_scrub_repl(const char *r);
const char *sp_str_encode(const char *s, sp_RbVal dst, sp_RbVal src, sp_RbVal invalid, sp_RbVal undef, sp_RbVal replace);
const char *sp_str_scrub_bang(const char *s, const char *repl);
const char*sp_str_ljust(const char*s,sp_int w);
const char*sp_str_rjust(const char*s,sp_int w);
const char*sp_str_center(const char*s,sp_int w);
const char*sp_str_ljust2(const char*s,sp_int w,const char*pad);
const char*sp_str_rjust2(const char*s,sp_int w,const char*pad);
const char*sp_str_center2(const char*s,sp_int w,const char*pad);
sp_oint sp_str_index_opt(const char *s, const char *sub);
sp_oint sp_str_index_from_opt(const char *s, const char *sub, sp_int start);
sp_oint sp_str_rindex_opt(const char *s, const char *sub);

/* ---- relocated from spinel_rt.h: hash key primitives (sp_str_hash /
   sp_str_eq / _sp_istr_idx) used by lib/sp_hash.c's StrInt/StrStr/IntStr/
   IntIntHash accessors. Still static (inline / noinline) -- each including
   TU (the single generated TU via spinel_rt.h, and sp_hash.c) gets its own
   private copy, so this is a pure textual relocation with no codegen change. ---- */
/* NULL-safe string equality. ENV[] returns NULL for unset vars
   (the dispatch is `sp_str_dup_external(getenv(...))`, which propagates
   NULL), so emitted strcmp(...) on the result of `ENV["X"] == "1"` would
   dereference NULL on either side. nil-vs-string equality is false in
   Ruby; nil == nil is true, so falling back to pointer equality on the
   NULL path covers both. */
static inline int sp_str_eq(const char*a,const char*b){
  if(a==b)return 1;
  if(!a||!b)return 0;
  if(strcmp(a,b)!=0)return 0;
  /* strcmp equality is only prefix equality when a length header records
     an embedded NUL ("a\0b" vs "a"): confirm byte-exact equality. The
     miss path above stays a single strcmp. */
  size_t la=sp_str_byte_len(a);
  if(la!=sp_str_byte_len(b)||memcmp(a,b,la)!=0)return 0;
  /* CRuby's rb_str_comparable: equal bytes are equal strings only when the
     encodings are comparable -- the same encoding, or both operands ASCII
     only. spinel has two, so the question is the BINARY tag; the ASCII scan
     runs only when the tags disagree about bytes that already matched, which
     keeps every ordinary comparison at the two reads above. */
  if(sp_str_is_binary(a)==sp_str_is_binary(b))return 1;
  for(size_t i=0;i<la;i++)if((unsigned char)a[i]>=0x80)return 0;
  return 1;
}
/* Compare a spinel string against a PLAIN C string (a stack buffer, a literal
   with no marker byte). sp_str_eq reads a marker in front of BOTH operands to
   recover a length that can carry an embedded NUL; handing it unmarked memory
   reads the byte before the object, and when that byte happens to look like a
   marker it reads a bogus header and answers a garbage length. The plain side
   has no embedded NUL by construction, so its length is strlen. */
static inline sp_bool sp_str_eq_cstr(const char *marked, const char *plain) {
  if (!marked || !plain) return (sp_bool)(marked == plain);
  size_t lp = strlen(plain);
  return (sp_bool)(sp_str_byte_len(marked) == lp && memcmp(marked, plain, lp) == 0);
}

/* String#valid_encoding? -- walks the buffer and accepts pure ASCII
   or well-formed UTF-8 (RFC 3629 byte sequences with no overlong
   forms, no surrogate halves, code points <= U+10FFFF). */
/* A BINARY string hashes apart from its UTF-8 twin, but only when it holds a
   byte that makes them incomparable: "abc".b and "abc" are eql? in CRuby and
   hash together, "caf\xC3\xA9" and "café" are neither. The high-byte question
   is answered inside the walk the hash already makes, so this costs one
   register. */
static inline uint64_t sp_str_hash_bytes(const char*s,int*saw_high){
  uint64_t h=14695981039346656037ULL;int hi=0;
  while(*s){unsigned char c=(unsigned char)*s++;hi|=(c>=0x80);h^=c;h*=1099511628211ULL;}
  *saw_high=hi;return h;
}
static inline uint64_t sp_str_hash_compute(const char*s){
  int hi=0;uint64_t h=sp_str_hash_bytes(s,&hi);
  if(hi&&sp_str_is_binary(s))h^=0x9e3779b97f4a7c15ULL;
  return h;
}
/* Cold path: compute (and, for a heap/heap-frozen string, cache) the FNV
   hash. Kept out-of-line so sp_str_hash's inline fast path -- a cached-hash
   read -- stays tiny and doesn't bloat every call site's code layout. */
static SP_NOINLINE uint64_t sp_str_hash_miss(const char*s,unsigned char m){
  if(m==0xfe||m==0xfc||m==0xf1||m==0xfa||m==0xf8){
    sp_str_hdr*hd=((sp_str_hdr*)(s-1))-1;
    uint64_t h=sp_str_hash_compute(s);
    hd->hash=h?h:1;
    return hd->hash;
  }
  return sp_str_hash_compute(s);
}
static inline uint64_t sp_str_hash(const char*s){
  /* nil hashes like any other absent key: a String-keyed lookup with a nil
     key answers "not present" in Ruby, and reading the tag byte at s[-1]
     off a NULL faulted instead (#3790). sp_str_eq already answers false
     against NULL, so the probe walks past every occupied slot. */
  if(!s)return 14695981039346656037ULL;
  unsigned char m=((const unsigned char*)s)[-1];
  if(m==0xfe||m==0xfc||m==0xf1||m==0xfa||m==0xf8){
    uint64_t cached=(((sp_str_hdr*)(s-1))-1)->hash;
    if(cached)return cached;
  }
  return sp_str_hash_miss(s,m);
}
static inline sp_int _sp_istr_idx(sp_int mask,sp_int k){return(sp_int)(((uint64_t)(unsigned long long)k*11400714819323198485ULL)&(uint64_t)mask);}

/* ---- more cold string ops relocated from spinel_rt.h (0 optcarrot
   uses; deps already visible via sp_str.h's own sp_array.h include). ---- */
sp_bool sp_str_in_list(const char *m, const char *const *list);
sp_RbVal sp_str_index_poly(const char *s, const char *sub);
sp_RbVal sp_str_index_from_poly(const char *s, const char *sub, sp_int start);
sp_RbVal sp_str_rindex_poly(const char *s, const char *sub);
sp_PolyArray *sp_str_lines_poly(const char *s);

#endif /* SP_STR_H */
