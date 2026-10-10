/* sp_str.c -- cold String transforms (see sp_str.h).
 *
 * Leaf `const char*` operations moved out of spinel_rt.h so they compile
 * once into libspinel_rt.a instead of into every generated TU. They reach
 * the GC string heap via sp_alloc.h and the typed arrays via sp_array.h;
 * sp_str_crypt is in lib/sp_str_crypt.c; sp_sprintf / sp_raise_* resolve
 * at the final link against the generated TU.
 *
 * The cold half (case mapping, strip / chomp / chop / succ, delete_prefix /
 * delete_suffix, sub / gsub, inspect / dump / undump and the Symbol name
 * helpers) is in lib/sp_str_cold.c. A unit's inlining budget is a share of
 * its size (--param inline-unit-growth), and with those bodies here gcc ran
 * out of it before the concat, plus and allocation helpers: sp_str_is_binary,
 * sp_str_alloc and sp_str_byte_len stopped inlining into them. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include "sp_str.h"
/* sp_re.h: what gsub / sub / scan with a String pattern leave in `$~`, when
   the program reads it (sp_re_track_last) */
extern int sp_re_track_last;
void sp_re_clear_last_match(void);
void sp_re_set_lit_match(const char *str, sp_int beg, sp_int end);


/* Single-char substring cache (sub_range fast path); per-process, only
   used by the sub_range helpers that live in this file. */
static char sp_char_cache[256][3];
static int sp_char_cache_init = 0;
/* The same table for a BINARY receiver, where the slice has to keep the tag.
   The tag lives in an sp_str_hdr, and the 0xff entries above have none -- so
   these entries carry one, under the 0xfb marker (static storage WITH a
   header). The header also gives a real length, so byte 0 is representable
   here where strlen makes it the empty string above. Built once; never
   allocated, never swept, never mutated in place (0xfb is not in the
   heap-string group any of those paths test for). */
typedef struct { sp_str_hdr h; char m; char d[2]; } sp_hdr_char_ent;
/* [0] plain (whatever the default encoding is -- spinel names one), [1] BINARY.
   Two tables rather than one because the tag is the only thing that differs and
   it has to be in the header the entry carries. */
static sp_hdr_char_ent sp_hdr_char_cache[2][256];
static int sp_hdr_char_cache_init = 0;
static const char *sp_hdr_char(unsigned char c, int binary) {
  if (!sp_hdr_char_cache_init) {
    for (int k = 0; k < 2; k++)
      for (int i = 0; i < 256; i++) {
        sp_hdr_char_cache[k][i].h.next = NULL;
        sp_hdr_char_cache[k][i].h.size = k ? SP_STR_SIZE_BINARY : 0u;
        sp_hdr_char_cache[k][i].h.len = 1;
        sp_hdr_char_cache[k][i].h.hash = 0;
        sp_hdr_char_cache[k][i].m = (char)0xfb;
        sp_hdr_char_cache[k][i].d[0] = (char)i;
        sp_hdr_char_cache[k][i].d[1] = 0;
      }
    sp_hdr_char_cache_init = 1;
  }
  return sp_hdr_char_cache[binary ? 1 : 0][c].d;
}
/* Integer#chr: a 1-byte string, no allocation. spinel does not let a program
   name an encoding, so nothing but pack and String#b asks for BYTES -- and a
   chr result is not one of those. It stays on the plain table, which is what
   it has always reported; CRuby would say US-ASCII (or ASCII-8BIT above 0x7F),
   a divergence spinel already accepts for want of a third encoding. */
const char *sp_plain_char(unsigned char c) { return sp_hdr_char(c, 0); }
const char *sp_bin_char(unsigned char c) { return sp_hdr_char(c, 1); }

/* Byte-oriented substring search (NUL-transparent). spinel Strings can carry
   embedded NULs (pack, socket reads), so the search family must use the header
   byte length rather than strstr's C-string scan, which stops at the first NUL
   and diverges from CRuby's byte-based #include?/#index (issue #1778). Returns
   a pointer into `hay`, or NULL. An empty needle matches at the start. */
const char *sp_bytestr(const char *hay, size_t hn, const char *need, size_t nn) {
  if (nn == 0) return hay;
  if (nn > hn) return NULL;
  for (size_t i = 0; i + nn <= hn; i++)
    if (hay[i] == need[0] && memcmp(hay + i, need, nn) == 0) return hay + i;
  return NULL;
}
int sp_utf8_set_has(const uint32_t*cps,size_t n,uint32_t cp){for(size_t i=0;i<n;i++)if(cps[i]==cp)return 1;return 0;}
/* Case-insensitive byte comparison over the REAL lengths: stopping at the
   first NUL made two strings differing only after one compare equal (#3471). */
sp_int sp_str_casecmp(const char*a,const char*b){if(!a)sp_nil_recv("casecmp");if(!b)return 1;
  size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b),n=la<lb?la:lb;
  for(size_t i=0;i<n;i++){int ca=tolower((unsigned char)a[i]),cb=tolower((unsigned char)b[i]);if(ca!=cb)return ca<cb?-1:1;}
  return la==lb?0:(la<lb?-1:1);}
/* ASCII-8BIT has no invalid byte sequence -- every byte is a character -- so a
   BINARY string is valid by definition and never walked. On the text side the
   walk is bounded by the byte length rather than by a terminator: a NUL is a
   valid UTF-8 character, and stopping there would call "a\0\xff" valid. */
sp_bool sp_str_valid_encoding(const char*s){if(!s)sp_nil_recv("valid_encoding?");
  if(sp_str_is_binary(s))return TRUE;
  const unsigned char*p=(const unsigned char*)s;const unsigned char*e=p+sp_str_byte_len(s);while(p<e){unsigned c=*p;if(c<0x80){p++;continue;}int extra;unsigned cp;unsigned min;if((c&0xE0)==0xC0){extra=1;cp=c&0x1F;min=0x80;}
else if((c&0xF0)==0xE0){extra=2;cp=c&0x0F;min=0x800;}
else if((c&0xF8)==0xF0){extra=3;cp=c&0x07;min=0x10000;}
else return FALSE;p++;for(int i=0;i<extra;i++){if(p>=e||(*p&0xC0)!=0x80)return FALSE;cp=(cp<<6)|(*p&0x3F);p++;}if(cp<min)return FALSE;if(cp>=0xD800&&cp<=0xDFFF)return FALSE;if(cp>0x10FFFF)return FALSE;}return TRUE;}
/* Extract the n-th field (0-based) from s split by sep, without
   allocating a full StrArray.  Returns a newly allocated string.
   If the field doesn't exist, returns "". */
const char*sp_str_field(const char*s,const char*sep,sp_int n){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sep);
  size_t sl=strlen(sep);sp_int cur=0;const char*p=s;
  if(sl==0)return sp_str_empty;
  while(cur<n){const char*f=strstr(p,sep);if(!f)return sp_str_empty;p=f+sl;cur++;}
  const char*end=strstr(p,sep);size_t len=end?((size_t)(end-p)):strlen(p);
  char*r=sp_str_alloc_raw(len+1);memcpy(r,p,len);r[len]=0;sp_str_set_len(r,len);return r;}
/* Count fields in s split by sep (without allocating). */
sp_int sp_str_field_count(const char*s,const char*sep){
  if(*s==0)return 0;
  size_t sl=strlen(sep);if(sl==0)return(sp_int)strlen(s);
  sp_int c=1;const char*p=s;while((p=strstr(p,sep))!=NULL){c++;p+=sl;}return c;}
/* The BINARY tag survives every operation that cannot change what the bytes
   are: a concatenation, a join, a repeat, a slice. CRuby's rule is that an
   ASCII-only operand adopts the other operand's encoding, so a result with any
   binary input is binary -- "hdr" + digest is ASCII-8BIT there, and a slice of
   a binary string is binary because a slice of bytes is bytes.

   Where CRuby would instead raise Encoding::CompatibilityError (two operands
   that BOTH carry bytes >= 0x80 in different encodings), spinel answers binary
   rather than raising; that is the existing posture of a runtime with two
   encodings, and answering text there was the bug -- an untagged result does
   not compare equal to the same bytes from pack, File.binread or a socket.

   Only freshly allocated results are marked. The shared empty string and other
   static returns are never touched: marking one would tag it for every other
   holder of the same pointer. */
static inline char *sp_str_bin_from(char *r, const char *a) {
  if (a && sp_str_is_binary(a)) sp_str_mark_binary(r);
  return r;
}

/* One step of CRuby's encoding rule over a string built part by part (an
   interpolation, Array#join): `state` is the encoding so far, -1 while only
   US-ASCII parts (an Integer's to_s, nothing yet) have gone in, 0 UTF-8, 1
   binary; `acc` holds the `acc_len` bytes already joined. A part in the same
   encoding, or an ASCII-only one, keeps it; one whose high bytes meet an
   ASCII-only run so far takes the run over; two runs with high bytes in
   different encodings stay binary (see above). */
int sp_str_enc_step(int state, const char *acc, size_t acc_len, const char *part, size_t part_len, int part_bin) {
  if (state < 0) return part_len ? part_bin : -1;
  if (part_bin == state || part_len == 0) return state;
  for (size_t i = 0; i < part_len; i++)
    if ((unsigned char)part[i] >= 0x80) {
      for (size_t j = 0; j < acc_len; j++) if ((unsigned char)acc[j] >= 0x80) return 1;
      return part_bin;
    }
  return state;
}
/* Whether a + b is binary, as CRuby's compatibility rule picks it: the
   operands' shared encoding, else the one an ASCII-only operand gives way
   to -- `String.new + "\u00e9"` is UTF-8 -- else binary (see above). */
static int sp_str_cat_binary(const char *a, const char *b) {
  int ab = sp_str_is_binary(a), bb = sp_str_is_binary(b);
  if (ab == bb) return ab;
  if (sp_str_ascii_only(b)) return ab;
  if (sp_str_ascii_only(a)) return bb;
  return 1;
}
/* The same rule folded over a run of parts: `bin` is the encoding of the
   `pl` bytes of r already joined, s the next part. The prefix is scanned only
   when the two encodings differ. */
static int sp_str_bin_fold(const char *r, size_t pl, int bin, const char *s) {
  int sb = sp_str_is_binary(s);
  if (sb == bin || sp_str_ascii_only(s)) return bin;
  for (size_t i = 0; i < pl; i++) if ((unsigned char)r[i] >= 0x80) return 1;
  return sb;
}
const char*sp_str_concat(const char*a,const char*b){SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);if(!a)a=sp_str_empty;if(!b)b=sp_str_empty;size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b);char*r=sp_str_alloc(la+lb);memcpy(r,a,la);memcpy(r+la,b,lb);if(sp_str_cat_binary(a,b))sp_str_mark_binary(r);return r;}
/* append_as_bytes keeps the receiver's encoding, regardless of the bytes or
   encoding of the appended String. */
const char*sp_str_append_bytes(const char*a,const char*b){SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);if(!a)a=sp_str_empty;if(!b)b=sp_str_empty;size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b);char*r=sp_str_alloc(la+lb);memcpy(r,a,la);memcpy(r+la,b,lb);return sp_str_bin_from(r,a);}
/* Issue #760: NULL src to memcpy is UB. Treat NULL as empty string. */
const char*sp_str_concat3(const char*a,const char*b,const char*c){SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);SP_GC_ROOT_STR(c);if(!a)a=sp_str_empty;if(!b)b=sp_str_empty;if(!c)c=sp_str_empty;size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b),lc=sp_str_byte_len(c);char*r=sp_str_alloc(la+lb+lc);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);{int ab=sp_str_cat_binary(a,b);int rb=ab==sp_str_is_binary(c)?ab:sp_str_ascii_only(c)?ab:(sp_str_ascii_only(a)&&sp_str_ascii_only(b))?sp_str_is_binary(c):1;if(rb)sp_str_mark_binary(r);}return r;}
const char*sp_str_concat4(const char*a,const char*b,const char*c,const char*d){SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);SP_GC_ROOT_STR(c);SP_GC_ROOT_STR(d);if(!a)a=sp_str_empty;if(!b)b=sp_str_empty;if(!c)c=sp_str_empty;if(!d)d=sp_str_empty;size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b),lc=sp_str_byte_len(c),ld=sp_str_byte_len(d);char*r=sp_str_alloc(la+lb+lc+ld);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);memcpy(r+la+lb+lc,d,ld);{int rb=sp_str_is_binary(a);rb=sp_str_bin_fold(r,la,rb,b);rb=sp_str_bin_fold(r,la+lb,rb,c);rb=sp_str_bin_fold(r,la+lb+lc,rb,d);if(rb)sp_str_mark_binary(r);}return r;}
/* Concatenate N strings into a single GC-managed buffer. */
/* Issue #760: NULL entries treated as empty strings. */
const char*sp_str_concat_arr(const char *const *parts,int n){size_t total=0;for(int i=0;i<n;i++)total+=sp_str_byte_len(parts[i]?parts[i]:sp_str_empty);char*r=sp_str_alloc(total);char*p=r;int rb=-1;for(int i=0;i<n;i++){const char*s=parts[i]?parts[i]:sp_str_empty;size_t sl=sp_str_byte_len(s);rb=rb<0?sp_str_is_binary(s):sp_str_bin_fold(r,(size_t)(p-r),rb,s);memcpy(p,s,sl);p+=sl;}if(rb>0)sp_str_mark_binary(r);return r;}
/* The unresolved-call gate's raise. Deliberately NOT declared noreturn
   (spinel_rt.h): gate arms sit inside hot dispatch functions and a noreturn
   call restructures their CFG; as a plain value-returning extern call the
   arm keeps the sp_box_nil() shape it replaced. sp_raise_cls longjmps, so
   the return never executes. */
sp_RbVal sp_raise_nomethod(const char *msg) {SP_GC_ROOT_STR(msg);
  sp_raise_cls("NoMethodError", msg);
  return sp_box_nil();
}
/* NoMethodError for a String method reaching a nil (NULL) receiver,
   matching CRuby's "undefined method 'upcase' for nil" message shape. */
void sp_nil_recv(const char*meth){SP_GC_ROOT_STR(meth);
  size_t cap=strlen(meth)+32;
  char*msg=sp_str_alloc_raw(cap);
  snprintf(msg,cap,"undefined method '%s' for nil",meth);
  SP_GC_ROOT_STR(msg);
  sp_raise_cls("NoMethodError",msg);
}
sp_int sp_str_length_m(const char*s){if(!s)sp_nil_recv("length");return sp_str_length(s);}
sp_int sp_str_bytesize_m(const char*s){if(!s)sp_nil_recv("bytesize");return (sp_int)sp_str_byte_len(s);}
/* A NUL is a byte, not a terminator: "\0abc" is four bytes long and not
   empty, and a binary string starting with one is ordinary. The first-byte
   test still decides every non-empty string in one load; only a leading NUL
   pays for the header read that knows the real length. */
sp_bool sp_str_empty_p(const char*s){if(!s)sp_nil_recv("empty?");return *s==0&&sp_str_byte_len(s)==0;}
const char*sp_str_plus(const char*a,const char*b){SP_GC_ROOT_STR(a);SP_GC_ROOT_STR(b);
  if(!a)sp_nil_recv("+");
  if(!b)sp_raise_cls("TypeError","no implicit conversion of nil into String");
  return sp_str_concat(a,b);
}
/* `str + "lit"`: the codegen knows the literal's byte length, and a source
   literal is never BINARY, so this skips the literal's header decode and
   encoding check, the nil-to-empty fixups, and the second call frame
   sp_str_plus spends before sp_str_concat. */
const char *sp_str_plus_lit(const char *a, const char *lit, size_t ll) {
  if (!a) sp_nil_recv("+");
  SP_GC_ROOT_STR(a);
  size_t la = sp_str_byte_len(a);
  char *r = sp_str_alloc(la + ll);
  memcpy(r, a, la);
  memcpy(r + la, lit, ll);
  /* a binary receiver keeps its encoding unless it is ASCII-only and the
     literal is not: then the result is the literal's UTF-8, as CRuby picks */
  if (sp_str_is_binary(a)) {
    int lit_ascii = 1;
    for (size_t i = 0; i < ll && lit_ascii; i++) lit_ascii = (unsigned char)lit[i] < 0x80;
    if (lit_ascii || !sp_str_ascii_only(a)) sp_str_mark_binary(r);
  }
  return r;
}
/* FrozenError naming the receiver, matching CRuby's
   "can't modify frozen String: \"abc\"" message shape. */
void sp_exc_stage_recv(sp_RbVal v);   /* generated TU: stages FrozenError#receiver */
void sp_raise_frozen_str(const char*s){SP_GC_ROOT_STR(s);const char*ins=sp_str_inspect(s);SP_GC_ROOT_STR(ins);const char*msg=sp_str_concat(&("\xff" "can't modify frozen String: ")[1],ins);SP_GC_ROOT_STR(msg);sp_exc_stage_recv(sp_box_str(s));sp_raise_cls("FrozenError",msg);}
/* Simple Unicode case mapping for ASCII + Latin-1 Supplement (U+00C0-00FF)
   + Latin Extended-A (U+0100-017F), the ranges the ruby/spec string case
   specs exercise. Full Unicode is out of scope (one internal UTF-8 rep). */
uint32_t sp_uc_toupper(uint32_t cp){
  if(cp<0x80)return (uint32_t)toupper((int)cp);
  if(cp>=0xE0&&cp<=0xFE&&cp!=0xF7)return cp-0x20; /* Latin-1 lower -> upper */
  if(cp==0xFF)return 0x178;                       /* ÿ -> Ÿ */
  if(cp>=0x100&&cp<=0x137){ if(cp&1)return cp-1; return cp; } /* Latin-Ext-A pairs */
  if(cp>=0x139&&cp<=0x148){ if((cp&1)==0)return cp-1; return cp; }
  if(cp>=0x14A&&cp<=0x177){ if(cp&1)return cp-1; return cp; }
  if(cp>=0x179&&cp<=0x17E){ if((cp&1)==0)return cp-1; return cp; }
  return cp;
}
uint32_t sp_uc_tolower(uint32_t cp){
  if(cp<0x80)return (uint32_t)tolower((int)cp);
  if(cp>=0xC0&&cp<=0xDE&&cp!=0xD7)return cp+0x20; /* Latin-1 upper -> lower */
  if(cp==0x178)return 0xFF;                        /* Ÿ -> ÿ */
  if(cp>=0x100&&cp<=0x137){ if((cp&1)==0)return cp+1; return cp; }
  if(cp>=0x139&&cp<=0x148){ if(cp&1)return cp+1; return cp; }
  if(cp>=0x14A&&cp<=0x177){ if((cp&1)==0)return cp+1; return cp; }
  if(cp>=0x179&&cp<=0x17E){ if(cp&1)return cp+1; return cp; }
  return cp;
}
/* `s << x`: append in place when the buffer has room, else move to a buffer
   with room to spare. The emitted form used to be `s = concat(s, x)`, a fresh
   exact-sized copy per append, so building a document one piece at a time was
   quadratic (an 8MB log took minutes). Only a heap string (0xfe/0xfc) is
   written in place; a literal or a frozen string is copied, and the caller has
   already raised on a frozen receiver. */
const char *sp_str_append_grow(const char *s, const char *t) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(t);
  if (!s) return t ? t : sp_str_empty;
  if (!t) return s;
  size_t la = sp_str_byte_len(s), lb = sp_str_byte_len(t);
  if (lb == 0) return s;
  int text_append = sp_str_is_binary(s) && !sp_str_is_binary(t) &&
                    !sp_str_ascii_only(t) && sp_str_ascii_only(s);
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    sp_str_hdr *h = ((sp_str_hdr *)(s - 1)) - 1;
    size_t total = (size_t)(h->size & SP_STR_SIZE_MASK);
    size_t cap = total > sizeof(sp_str_hdr) + 2 ? total - sizeof(sp_str_hdr) - 2 : 0;
    if (la + lb <= cap) {
      memcpy((char *)s + la, t, lb);
      ((char *)s)[la + lb] = 0;
      sp_str_lcache_drop(s);
      sp_str_set_len((char *)s, la + lb);
      if (text_append) sp_str_as_text((char *)s);
      return s;
    }
  }
  size_t want = (la + lb) * 2 + 16;
  char *r = sp_str_alloc(want);
  memcpy(r, s, la);
  memcpy(r + la, t, lb);
  r[la + lb] = 0;
  sp_str_set_len(r, la + lb);
  /* Preserve BINARY when growing the receiver. A compatible text append to
     an ASCII-only binary receiver is the exception: text_append has already
     selected UTF-8, so do not mark the replacement buffer binary. */
  if (sp_str_is_binary(s) && !text_append) sp_str_mark_binary(r);
  return r;
}
/* The same append for the first `lb` bytes of `t`, for the append form of an
   interpolation (emit_interp_append): the length was taken when the part was
   evaluated, so a part that is `s` itself contributes what it held then even
   if an earlier part's append grew `s` in place. */
const char *sp_str_append_grow_n(const char *s, const char *t, size_t lb) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(t);
  if (!s) return t ? sp_str_from_bytes(t, lb) : sp_str_empty;
  if (!t || lb == 0) return s;
  size_t la = sp_str_byte_len(s);
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    sp_str_hdr *h = ((sp_str_hdr *)(s - 1)) - 1;
    size_t total = (size_t)(h->size & SP_STR_SIZE_MASK);
    size_t cap = total > sizeof(sp_str_hdr) + 2 ? total - sizeof(sp_str_hdr) - 2 : 0;
    if (la + lb <= cap) {
      memmove((char *)s + la, t, lb);   /* t may point into s (a self-append) */
      ((char *)s)[la + lb] = 0;
      sp_str_lcache_drop(s);
      sp_str_set_len((char *)s, la + lb);
      return s;
    }
  }
  size_t want = (la + lb) * 2 + 16;
  char *r = sp_str_alloc(want);
  memcpy(r, s, la);
  memcpy(r + la, t, lb);
  r[la + lb] = 0;
  sp_str_set_len(r, la + lb);
  if (sp_str_is_binary(s)) sp_str_mark_binary(r);
  return r;
}
const char*sp_str_substr(const char*s,sp_int start,sp_int len){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("[]");if(len<=0){char*r=sp_str_alloc_raw(1);r[0]=0;sp_str_set_len(r,0);return sp_str_bin_from(r,s);}if(start<0)start=0;char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;sp_str_set_len(r,(size_t)len);return sp_str_bin_from(r,s);}
/* String#chr: the first character (a whole UTF-8 char, not a byte), "" for "" (#3083). */
const char*sp_str_chr(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("chr");if(*s==0)return sp_str_empty;int n=sp_utf8_advance(s);char*r=sp_str_alloc_raw((size_t)n+1);memcpy(r,s,(size_t)n);r[n]=0;sp_str_set_len(r,(size_t)n);return r;}
/* The character index at byte offset `byteoff`; preserves -1 (no match) and 0.
   Used to report a regexp match position in characters, not bytes (#3056). */
sp_int sp_str_byte_to_char(const char*s,sp_int byteoff){if(byteoff<=0||!s)return byteoff;sp_int bl=(sp_int)sp_str_byte_len(s);sp_int ci=0;for(sp_int i=0;i<byteoff&&i<bl;ci++)i+=sp_utf8_advance(s+i);/* to the byte length, not the first NUL (#4527) */return ci;}
/* Issue #797: NULL guards on receiver + needle for the chunk of
   string functions that read directly into a non-checked strlen. */
sp_bool sp_str_include(const char*s,const char*sub){if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)sp_nil_recv("include?");return sp_bytestr(s,sp_str_byte_len(s),sub,sp_str_byte_len(sub))!=NULL;}
sp_bool sp_str_start_with(const char*s,const char*p){if(!p)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)sp_nil_recv("start_with?");size_t lp=sp_str_byte_len(p);return lp<=sp_str_byte_len(s)&&memcmp(s,p,lp)==0;}
sp_bool sp_str_end_with(const char*s,const char*suf){if(!suf)sp_raise_cls("TypeError","no implicit conversion of nil into String");if(!s)sp_nil_recv("end_with?");size_t ls=sp_str_byte_len(s),lsuf=sp_str_byte_len(suf);/* byte-exact: a NUL is a byte, not the end (#4527) */if(lsuf>ls)return FALSE;return memcmp(s+ls-lsuf,suf,lsuf)==0;}
/* partition: [before, sep, after] at the first sep; no match -> [s, "", ""]. */
/* partition: [before, sep, after] at the first sep; no match -> [s, "", ""]. */
sp_StrArray *sp_str_partition(const char *s, const char *sep) {
  if (!sep) sp_raise_cls("TypeError", "wrong argument type nil (expected Regexp)");
  SP_GC_ROOT_STR(s); SP_GC_ROOT_STR(sep);
  sp_StrArray *r = sp_StrArray_new();
  SP_GC_ROOT(r);   /* keep r (and its pushed slices) live across the byteslice allocs */
  /* byte-exact: strlen/strstr cannot express a NUL in the separator or the
     subject, so partitioning on one found nothing and answered ["", "", s] */
  sp_int bl = (sp_int)sp_str_byte_len(s), sl = (sp_int)sp_str_byte_len(sep);
  const char *f = sl > 0 ? sp_bytestr(s, (size_t)bl, sep, (size_t)sl) : s;
  if (!f) { sp_StrArray_push(r, sp_str_dup(s)); sp_StrArray_push(r, sp_str_dup(sp_str_empty)); sp_StrArray_push(r, sp_str_dup(sp_str_empty)); return r; }
  sp_int pre = (sp_int)(f - s);
  sp_StrArray_push(r, sp_str_byteslice(s, 0, pre));
  sp_StrArray_push(r, sp_str_byteslice(s, pre, sl));
  sp_StrArray_push(r, sp_str_byteslice(s, pre + sl, bl - pre - sl));
  return r;
}
/* rpartition: split at the last sep; no match -> ["", "", s]. */
/* rpartition: split at the last sep; no match -> ["", "", s]. */
sp_StrArray *sp_str_rpartition(const char *s, const char *sep) {
  if (!sep) sp_raise_cls("TypeError", "wrong argument type nil (expected Regexp)");
  SP_GC_ROOT_STR(s); SP_GC_ROOT_STR(sep);
  sp_StrArray *r = sp_StrArray_new();
  SP_GC_ROOT(r);   /* keep r (and its pushed slices) live across the byteslice allocs */
  sp_int bl = (sp_int)sp_str_byte_len(s), sl = (sp_int)sp_str_byte_len(sep);
  const char *last = sl == 0 ? s + bl : NULL;   /* an empty sep matches at the end: [s, "", ""] */
  if (sl > 0) { const char *p = s, *e = s + bl;
    while (p < e) { const char *f2 = sp_bytestr(p, (size_t)(e - p), sep, (size_t)sl); if (!f2) break; last = f2; p = f2 + 1; } }
  if (!last) { sp_StrArray_push(r, sp_str_dup(sp_str_empty)); sp_StrArray_push(r, sp_str_dup(sp_str_empty)); sp_StrArray_push(r, sp_str_dup(s)); return r; }
  sp_int pre = (sp_int)(last - s);
  sp_StrArray_push(r, sp_str_byteslice(s, 0, pre));
  sp_StrArray_push(r, sp_str_byteslice(s, pre, sl));
  sp_StrArray_push(r, sp_str_byteslice(s, pre + sl, bl - pre - sl));
  return r;
}
/* String#lines: split on \n but PRESERVE the trailing newline on each
   line (CRuby semantics). The last line keeps its terminator if present;
   if absent, it just stops there. Empty string returns an empty array.
   `end` is computed once at entry so a string with no newlines avoids
   a redundant strlen call on the trailing piece. */
/* String#lines(sep) / #each_line(sep): segments each keeping the separator
   (the trailing fragment keeps whatever remains). An empty sep is CRuby's
   "paragraph mode" -- not supported here; callers gate on a non-empty sep. */
/* lines(sep, chomp: true): the separator's run is trimmed off each piece
   (#3546) */
sp_StrArray*sp_str_lines_sep_chomp(const char*s,const char*sep){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sep);
  sp_StrArray*src=sp_str_lines_sep(s,sep);SP_GC_ROOT(src);
  sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);
  size_t sl=sep?sp_str_byte_len(sep):0;
  for(sp_int i=0;i<sp_StrArray_length(src);i++){
    const char*e=sp_StrArray_get(src,i);size_t n=e?sp_str_byte_len(e):0;
    if(sl==0){while(n>0&&e[n-1]=='\n')n--;}
    else if(n>=sl&&memcmp(e+n-sl,sep,sl)==0)n-=sl;
    char*r=sp_str_alloc_raw(n+1);memcpy(r,e,n);r[n]=0;sp_str_set_len(r,n);
    sp_StrArray_push(a,r);
  }
  return a;
}
sp_StrArray*sp_str_lines_sep(const char*s,const char*sep){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sep);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);if(!s||!sp_str_byte_len(s))return a;size_t sl=sep?sp_str_byte_len(sep):0;/* an empty separator is CRuby's paragraph mode: split after each run of two
   or more newlines, keeping the run with the paragraph (#3546) */
if(sl==0){const char*end0=s+sp_str_byte_len(s);const char*p0=s;while(p0<end0){const char*q=p0;while(q<end0){if(*q=='\n'){const char*b1=q;while(b1<end0&&*b1=='\n')b1++;if(b1-q>=2)break;q=b1;continue;}q++;}size_t n0=(q>=end0)?(size_t)(end0-p0):(size_t)(q-p0)+2;char*r0=sp_str_alloc_raw(n0+1);memcpy(r0,p0,n0);r0[n0]=0;sp_str_set_len(r0,n0);sp_StrArray_push(a,sp_str_bin_like(s,r0));if(q>=end0)break;{const char*b2=q;while(b2<end0&&*b2=='\n')b2++;p0=b2;}}return a;}const char*end=s+sp_str_byte_len(s);const char*p=s;while(p<end){const char*hit=sp_bytestr(p,(size_t)(end-p),sep,sl);size_t n=hit?(size_t)(hit-p)+sl:(size_t)(end-p);char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_str_set_len(r,n);sp_StrArray_push(a,sp_str_bin_like(s,r));if(!hit)break;p=hit+sl;}return a;}
sp_StrArray*sp_str_lines(const char*s){SP_GC_ROOT_STR(s);sp_StrArray*a=sp_StrArray_new();size_t bl=sp_str_byte_len(s);if(bl==0)return a;SP_GC_ROOT(a);const char*end=s+bl;const char*p=s;while(p<end){const char*nl=memchr(p,'\n',(size_t)(end-p));size_t n=nl?(size_t)(nl-p+1):(size_t)(end-p);char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_str_set_len(r,n);sp_StrArray_push(a,sp_str_bin_like(s,r));if(!nl)break;p=nl+1;}return a;}
sp_StrArray*sp_str_lines_chomp(const char*s){SP_GC_ROOT_STR(s);sp_StrArray*a=sp_StrArray_new();size_t bl=sp_str_byte_len(s);if(bl==0)return a;SP_GC_ROOT(a);const char*end=s+bl;const char*p=s;while(p<end){const char*nl=memchr(p,'\n',(size_t)(end-p));size_t n=nl?(size_t)(nl-p):(size_t)(end-p);if(nl&&nl>s&&nl[-1]=='\r')n--;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_str_set_len(r,n);sp_StrArray_push(a,r);if(!nl)break;p=nl+1;}return a;}
/* String#byteslice(start,len): byte-indexed (unlike the char-indexed
   sp_str_sub_range). Negative start counts back from the byte length.
   Out-of-range yields the empty string rather than CRuby nil. */
const char*sp_str_byteslice(const char*s,sp_int start,sp_int len){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("byteslice");sp_int bl=(sp_int)sp_str_byte_len(s);if(start<0)start+=bl;if(start<0||start>bl||len<0){return NULL;/* out of range -> nil (#2333) */}if(len>bl-start)len=bl-start;if(len<=0){return sp_str_is_binary(s)?sp_str_empty_binary():&("\xff" "")[1];}char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;sp_str_set_len(r,(size_t)len);return sp_str_bin_from(r,s);}
/* Single-argument String#byteslice(i): the 1-byte substring at byte index i,
   or nil when i is outside [0, bytesize) (the boundary i == bytesize has no
   byte, unlike the two-argument form's empty slice) (#2333). */
const char*sp_str_byteslice1(const char*s,sp_int i){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("byteslice");sp_int bl=(sp_int)sp_str_byte_len(s);if(i<0)i+=bl;if(i<0||i>=bl)return NULL;return sp_str_byteslice(s,i,1);}
/* String#byteslice(range): resolve beginless/endless/negative endpoints
   against the bytesize, then slice; a begin past the bytesize is nil (#2348). */
const char*sp_str_byteslice_range(const char*s,sp_int lo,sp_int hi,int excl,int lo_none,int hi_none){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("byteslice");sp_int bl=(sp_int)sp_str_byte_len(s);if(lo_none)lo=0;else if(lo<0)lo+=bl;if(lo<0||lo>bl)return NULL;sp_int end;if(hi_none)end=bl;else{end=hi;if(end<0)end+=bl;if(!excl)end+=1;}if(end>bl)end=bl;sp_int len=end-lo;if(len<0)len=0;return sp_str_byteslice(s,lo,len);}
/* String#bytesplice(start,len,str): replace the byte span with str and
   return the new value (the codegen arm rebinds the receiver, so the
   result stands in for CRuby's mutated self). CRuby's IndexError contracts:
   negative length raises, start is checked against the byte length with
   the original (pre-normalization) index in the message; an overlong span
   clamps to the end. */
const char*sp_str_bytesplice(const char*s,sp_int start,sp_int len,const char*val){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(val);if(!s)sp_nil_recv("bytesplice");if(!val)val=&("\xff" "")[1];sp_int bl=(sp_int)sp_str_byte_len(s);if(len<0)sp_raise_cls("IndexError",sp_sprintf("negative length %lld",(long long)len));sp_int st=start<0?start+bl:start;if(st<0||st>bl)sp_raise_cls("IndexError",sp_sprintf("index %lld out of string",(long long)start));if(len>bl-st)len=bl-st;size_t vl=sp_str_byte_len(val);char*r=sp_str_alloc_raw((size_t)(bl-len)+vl+1);memcpy(r,s,(size_t)st);memcpy(r+st,val,vl);memcpy(r+st+vl,s+st+len,(size_t)(bl-st-len));r[(size_t)(bl-len)+vl]=0;sp_str_set_len(r,(size_t)(bl-len)+vl);return r;}
/* String#ascii_only?: 1 iff every byte is in the 7-bit ASCII range. */
int sp_str_ascii_only(const char*s){sp_int bl=(sp_int)sp_str_byte_len(s);for(sp_int i=0;i<bl;i++){if((unsigned char)s[i]>=0x80)return 0;}return 1;}
const char*sp_str_format_strarr(const char*fmt,sp_StrArray*a){SP_GC_ROOT_STR(fmt);size_t cap=strlen(fmt)+64;char*buf=(char*)malloc(cap);if(!buf){perror("malloc");exit(1);}size_t out=0;sp_int idx=0;const char*p=fmt;while(*p){if(*p=='%'){if(p[1]=='s'){const char*s=(idx<a->len)?a->data[idx]:"";size_t sl=sp_str_byte_len(s);   /* the byte length: an embedded NUL is a byte of the string (#4632) */if(out+sl>=cap){size_t nc=((out+sl)*2)+1;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}memcpy(buf+out,s,sl);out+=sl;idx++;p+=2;}
else if(p[1]=='%'){if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]='%';p+=2;}
else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}
else{if(out+1>=cap){size_t nc=cap*2;char*nb=(char*)realloc(buf,nc);if(!nb){free(buf);perror("realloc");exit(1);}buf=nb;cap=nc;}buf[out++]=*p++;}}buf[out]=0;char*r=sp_str_alloc(out);memcpy(r,buf,out);free(buf);return r;}
/* The longest string a caller-sized operation builds: String#*, and the
   center/ljust/rjust family. Past it the request raises ArgumentError, which a
   program can rescue, instead of reaching the allocator -- a failed
   allocation ends the process (sp_oom_die) -- and sp_str_hdr's 32-bit length
   fields never see a value they would truncate. `have` is what the result
   already holds, `add` what the operation appends. */
static const size_t sp_str_max_bytes=(size_t)1<<30;
static void sp_str_check_size(size_t have, size_t add){if(have>sp_str_max_bytes||add>sp_str_max_bytes-have)sp_raise_cls("ArgumentError","string size too big");}
/* The justify family's size rule, in 64 bits on every target, as CRuby
   answers it: a byte total past a long is ArgumentError ("argument too big");
   one past what this runtime builds (sp_str_max_bytes) is the NoMemoryError
   CRuby raises when the allocation fails -- raised here, so it can be rescued
   and the allocator never sees the request. UINT64_MAX stands for a total
   that overflowed. Answers the class to raise, NULL when the result fits. */
static const char *sp_justify_size_err(uint64_t have, uint64_t add) {
  if (add == UINT64_MAX || add >= (uint64_t)INT64_MAX - have) return "ArgumentError";
  if (have + add > (uint64_t)sp_str_max_bytes) return "NoMemoryError";
  return NULL;
}
static void sp_justify_size_raise(const char *cls) {
  sp_raise_cls(cls, cls[0] == 'A' ? "argument too big" : "failed to allocate memory");
}
/* The bytes `need` characters of a cycling pad take, counted per cycle
   rather than per character; UINT64_MAX on overflow. */
static uint64_t sp_pad_bytes(const uint32_t *cps, size_t pn, uint64_t need) {
  char tmp[4]; uint64_t cyc = 0, part = 0, rem = need % pn;
  for (size_t i = 0; i < pn; i++) {
    uint64_t b = (uint64_t)sp_utf8_encode(cps[i], tmp);
    cyc += b; if (i < rem) part += b;
  }
  uint64_t full = need / pn;
  if (cyc && full > (UINT64_MAX - part) / cyc) return UINT64_MAX;
  return full * cyc + part;
}
const char*sp_str_repeat(const char*s,sp_int n){SP_GC_ROOT_STR(s);
  if(n<0) sp_raise_cls("ArgumentError","negative argument");
  if(!s)sp_nil_recv("*");if(n<=0)return sp_str_is_binary(s)?sp_str_empty_binary():sp_str_empty;
  /* the real byte length, not strlen: `0.chr * 3` is three NUL bytes, and a
     repeat of "a\0b" carries the NUL through each copy (#3473) */
  size_t l=sp_str_byte_len(s);
  if(l==0) return sp_str_is_binary(s)?sp_str_empty_binary():sp_str_empty;
  if((size_t)n>SIZE_MAX/l) sp_raise_cls("ArgumentError","string size too big");
  size_t total=(size_t)n*l;
  sp_str_check_size(0,total);
  char*r=sp_str_alloc_raw(total+1);
  for(sp_int i=0;i<n;i++)memcpy(r+(l*i),s,l);
  r[total]=0;
  sp_str_set_len(r,total);
  return sp_str_bin_from(r,s);
}
/* root `s` before the IntArray_new GC-alloc: the argument is often a fresh
   unrooted temp (`data[off, n].bytes` in a fused times.map -- doom's
   Colormap.load), and a collection triggered by the alloc freed it mid-call,
   yielding an empty result exactly on GC-boundary iterations. */
sp_IntArray*sp_str_bytes(const char*s){SP_GC_ROOT_STR(s);sp_IntArray*a=sp_IntArray_new();if(!s)sp_nil_recv("bytes");size_t n=sp_str_byte_len(s);for(size_t i=0;i<n;i++)sp_IntArray_push(a,(sp_int)(unsigned char)s[i]);return a;}
const char*sp_str_lstrip(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("lstrip");size_t len=sp_str_byte_len(s);size_t a=0;while(a<len&&(isspace((unsigned char)s[a])||s[a]=='\0'))a++;size_t n=len-a;char*r=sp_str_alloc(n);memcpy(r,s+a,n);r[n]=0;return sp_str_bin_like(s,r);}
const char*sp_str_rstrip(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("rstrip");size_t len=sp_str_byte_len(s);size_t b=len;while(b>0&&(isspace((unsigned char)s[b-1])||s[b-1]=='\0'))b--;char*r=sp_str_alloc(b);memcpy(r,s,b);r[b]=0;return sp_str_bin_like(s,r);}
/* String#b: a fresh, unfrozen copy tagged BINARY (ASCII-8BIT). The dup alone
   left the copy UTF-8, so `"caf\u00e9".b.inspect` printed the characters where
   CRuby prints the escaped bytes, and length kept counting characters. pack
   already marks its answer; #b is the sibling that did not. */
const char*sp_str_b(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("b");char*r=(char*)sp_str_dup(s);sp_str_mark_binary(r);return r;}
const char*sp_str_dup(const char*s){SP_GC_ROOT_STR(s);if(!s)return NULL;size_t l=sp_str_byte_len(s);char*r=sp_str_alloc(l);memcpy(r,s,l);if(sp_str_is_binary(s))sp_str_mark_binary(r);return r;}

/* ===================== utf8-dependent transforms ===================== */
sp_int sp_str_count_chars(const char *s, size_t bl) {
  const char *p = s, *end = s + bl;
  sp_int n = 0;
  while (p + 8 <= end) {
    uint64_t w;
    memcpy(&w, p, sizeof(w));
    if (w & 0x8080808080808080ULL) break;
    p += 8; n += 8;
  }
  while (p < end) {
    if ((unsigned char)*p < 0x80) { p++; n++; }
    else { p += sp_utf8_advance(p); n++; }
  }
  return n;
}
/* Length in "units" for String#length/size, validating and counting in a
   single walk: count UTF-8 codepoints while the bytes stay valid UTF-8, but
   fall back to the raw byte count the moment an invalid byte appears
   (near-certain for genuine binary data, e.g. a binary-mode File.read of a WAD
   lump) -- matching Ruby's ASCII-8BIT semantics for binary I/O without a
   separate per-string encoding tag. Folding validation into the count keeps
   String#length off a two-pass hot path; valid text still costs one walk. */
static sp_int sp_str_count_units(const char *s, size_t bl) {
  const unsigned char *p = (const unsigned char *)s, *end = p + bl;
  sp_int n = 0;
  for (;;) {
    /* ASCII fast path: 8 bytes at a time, each a valid single-unit codepoint */
    while (p + 8 <= end) {
      uint64_t w;
      memcpy(&w, p, sizeof(w));
      if (w & 0x8080808080808080ULL) break;
      p += 8; n += 8;
    }
    if (p >= end) break;
    unsigned c = *p;
    if (c < 0x80) { p++; n++; continue; }
    int extra; unsigned cp, min;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
    else return (sp_int)bl;   /* invalid lead byte -> count bytes */
    p++;
    if (p + extra > end) return (sp_int)bl;
    for (int i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) return (sp_int)bl;
      cp = (cp << 6) | (*p & 0x3F);
      p++;
    }
    if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return (sp_int)bl;
    n++;
  }
  return n;
}
sp_int sp_str_length(const char*s){
  if (!s) return 0;
  /* binary bytes are one unit each, whatever they happen to spell in UTF-8;
     a string a scan has proved 7-bit counts the same way, and answering from
     the header skips the pointer-keyed cache probe entirely */
  { size_t fb; if (sp_str_fixed_width(s, &fb)) return (sp_int)fb; }
  if (sp_str_is_binary(s)) return (sp_int)sp_str_byte_len(s);
  if (!sp_str_cacheable(s)) return sp_str_count_units(s, sp_str_byte_len(s));
  unsigned h = sp_str_lcache_hash(s);
  for (unsigned w = 0; w < SP_STR_LCACHE_WAYS; w++)
    if (sp_str_lcache[h + w].s == s) return sp_str_lcache[h + w].char_len;
  size_t bl = sp_str_byte_len(s);
  sp_int n = sp_str_count_units(s, bl);
  /* the walk just told us: char count == byte count means every byte is
     below 0x80, so remember it in the header and never walk (or probe) again */
  if ((size_t)n == bl) sp_str_mark_ascii7(s);
  /* Evict the cheapest entry to recompute, not the oldest: a scan over one long
     subject allocates a stream of short strings, and measuring those pushed the
     subject out of its bucket over and over -- each miss another walk of the
     whole thing. A short newcomer never displaces a much longer incumbent. */
  unsigned victim = h;
  for (unsigned w = 1; w < SP_STR_LCACHE_WAYS; w++) {
    if (!sp_str_lcache[h + w].s) { victim = h + w; break; }
    if (sp_str_lcache[h + w].byte_len < sp_str_lcache[victim].byte_len) victim = h + w;
  }
  if (sp_str_lcache[victim].s && sp_str_lcache[victim].byte_len > bl * 8) return n;
  sp_str_lcache[victim].s = s;
  sp_str_lcache[victim].byte_len = bl;
  sp_str_lcache[victim].char_len = n;
  return n;
}
/* The byte length comes from sp_str_byte_len, which knows every marker: this
   spelled out 0xfe/0xfc itself and fell to strlen for the rest, so a
   header-bearing string whose first byte is NUL -- a 1-byte slice of a BINARY
   string, served from the static table -- measured 0 and raised. */
sp_int sp_str_ord(const char*s){if(!s)sp_nil_recv("ord");
  if(sp_str_byte_len(s)==0)sp_raise_cls("ArgumentError","empty string");
  uint32_t cp;sp_utf8_decode(s,&cp);return(sp_int)cp;}
size_t sp_utf8_byte_offset(const char*s,sp_int char_idx){
  if (!s || char_idx <= 0) return 0;
  /* one unit per byte on a binary string, so indexing and slicing agree with
     the byte-counted #length */
  { size_t fb;
    if (sp_str_fixed_width(s, &fb)) { size_t off = (size_t)char_idx; return off > fb ? fb : off; } }
  if (sp_str_is_binary(s)) {
    size_t bl = sp_str_byte_len(s), off = (size_t)char_idx;
    return off > bl ? bl : off;
  }
  if (sp_str_cacheable(s)) {
    unsigned h = sp_str_lcache_hash(s);
    for (unsigned w = 0; w < SP_STR_LCACHE_WAYS; w++) {
      if (sp_str_lcache[h + w].s != s) continue;
      if ((size_t)sp_str_lcache[h + w].char_len != sp_str_lcache[h + w].byte_len) break;
      size_t off = (size_t)char_idx;
      return off > sp_str_lcache[h + w].byte_len ? sp_str_lcache[h + w].byte_len : off;
    }
  }
  /* Walk char_idx code points or stop at byte_len, whichever comes first.
     Bounding on byte_len (instead of "*p != 0") keeps the walk correct
     past an embedded NUL byte -- a heap string with the 0xfe/0xfc marker
     carries its real length in the sp_str_hdr, so a NUL byte inside the
     payload no longer terminates the walk prematurely. For an external
     0xff literal sp_str_byte_len falls back to strlen which legitimately
     stops at NUL, matching the prior behaviour. */
  size_t blen = sp_str_byte_len(s);
  const char *p = s;
  const char *end = s + blen;
  while (char_idx > 0 && p < end) {
    p += sp_utf8_advance(p);
    char_idx--;
  }
  if (p > end) p = end;
  return (size_t)(p - s);
}
uint32_t*sp_utf8_decode_all(const char*s,size_t*out_n){size_t cap=8,n=0;uint32_t*cps=(uint32_t*)malloc(cap*sizeof(uint32_t));if(!cps){*out_n=0;return NULL;}const char*p=s;const char*end=s?s+sp_str_byte_len(s):s;/* byte-exact: a NUL is a code point (#4527) */while(s&&p<end){if(n>=cap){size_t nc=cap*2;uint32_t*nx=(uint32_t*)realloc(cps,nc*sizeof(uint32_t));if(!nx){free(cps);*out_n=0;return NULL;}cps=nx;cap=nc;}uint32_t cp;p+=sp_utf8_decode(p,&cp);cps[n++]=cp;}*out_n=n;return cps;}
/* Length-aware variant: honors `bl` bytes so a NUL in the charset (e.g.
   `"a\0b".count(itself)`) is a real member rather than a terminator. */
uint32_t*sp_utf8_decode_charset_n(const char*s,size_t bl,size_t*out_n){SP_GC_ROOT_STR(s);
  size_t cap=16,n=0;
  uint32_t*cps=(uint32_t*)malloc(cap*sizeof(uint32_t));
  if(!cps){*out_n=0;return NULL;}
  const char*p=s; const char*end=s+bl;
  uint32_t prev=0; int has_prev=0;
  while(s&&p<end){
    uint32_t cp;
    int len=sp_utf8_decode(p,&cp);
    p+=len;
    /* A backslash escapes the next character, so `"a\\-b"` is the three
       members a, - and b rather than the range a..b (#3552). */
    if(cp=='\\'&&p<end){
      int elen=sp_utf8_decode(p,&cp);
      p+=elen;
      if(n>=cap){cap*=2;cps=(uint32_t*)realloc(cps,cap*sizeof(uint32_t));}
      cps[n++]=cp;
      prev=cp; has_prev=1;
      continue;
    }
    /* Detect range: prev '-' next  (but leading or trailing '-'
       is literal). When current char is '-' and there's a next
       non-'-' char and we have a prev, expand. */
    if(cp=='-' && has_prev && p<end){
      uint32_t hi;
      int hi_len=sp_utf8_decode(p,&hi);
      p+=hi_len;
      if(hi>=prev){
        /* Drop the prev we already wrote, re-emit the whole range. */
        n--;  /* undo prev */
        for(uint32_t c=prev;c<=hi;c++){
          if(n>=cap){cap*=2;cps=(uint32_t*)realloc(cps,cap*sizeof(uint32_t));}
          cps[n++]=c;
        }
        has_prev=0;
        continue;
      }
      /* Descending range (hi < prev) is an error in count/tr/delete/squeeze. */
      free(cps);
      sp_raise_cls("ArgumentError", "invalid range in string transliteration");
    }
    if(n>=cap){cap*=2;cps=(uint32_t*)realloc(cps,cap*sizeof(uint32_t));}
    cps[n++]=cp;
    prev=cp; has_prev=1;
  }
  *out_n=n;
  return cps;
}
uint32_t*sp_utf8_decode_charset(const char*s,size_t*out_n){SP_GC_ROOT_STR(s);return sp_utf8_decode_charset_n(s,s?strlen(s):0,out_n);}
/* Reuse an existing StrArray for split, avoiding GC alloc.
   Clears a->len and refills.  Substring strings are still malloc'd. */
void sp_str_split_into(sp_StrArray*a,const char*s,const char*sep){
  SP_GC_ROOT(a);
  SP_GC_ROOT_STR(s);
  SP_GC_ROOT_STR(sep);
  a->len=0;
  /* byte lengths and a byte-exact search: strlen/strstr stop at an embedded
     NUL, so a subject or separator holding one split short (the opportunistic
     NUL policy -- fix what is met). */
  size_t bl=sp_str_byte_len(s);
  if(bl==0)return;
  size_t sl=sp_str_byte_len(sep);
  if(sl==0){
    for(size_t i=0;i<bl;){
      int cn=sp_utf8_advance(s+i); if(cn<1)cn=1; if((size_t)cn>bl-i)cn=(int)(bl-i);
      sp_str_split_push(a,s+i,(size_t)cn);
      i+=(size_t)cn;
    }
    return;
  }
  const char*p=s;const char*se=s+bl;
  while(1){
    const char*f=sp_bytestr(p,(size_t)(se-p),sep,sl);
    if(!f){
      sp_str_split_push(a,p,(size_t)(se-p));
      break;
    }
    size_t n=(size_t)(f-p);
    sp_str_split_push(a,p,n);
    p=f+sl;
  }
}
sp_StrArray*sp_str_split(const char*s,const char*sep){if(!s)sp_nil_recv("split");
  SP_GC_ROOT_STR(s);
  SP_GC_ROOT_STR(sep);
  sp_StrArray*a=sp_StrArray_new();
  sp_str_split_into(a,s,sep);
  return a;
}
/* Same as sp_str_split but removes trailing empty strings
   (CRuby default limit behavior: split without limit drops
   trailing empties; split(sep, -1) keeps them). */
sp_StrArray*sp_str_split_drop_trailing(const char*s,const char*sep){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sep);if(!sep||(sep[0]==' '&&sp_str_byte_len(sep)==1))return sp_str_split_ws(s);sp_StrArray*a=sp_str_split(s,sep);/* an EMPTY tail, not one that merely starts with NUL: `"\0x\0".split("x")`
     dropped both halves when the test was s[0]==0. */
  while(a->len>0&&sp_str_byte_len(a->data[a->len-1])==0)a->len--;return a;}
/* `s.split(sep, n)` with explicit limit. Positive n caps the result
   at n elements: the last element holds the unsplit remainder.
   n == 0 means "no limit" and drops trailing empty strings (same as
   the no-arg default); n < 0 means "no limit" but keeps trailing
   empties. Empty separator splits into Unicode characters, then keeps
   one empty field after them if the limit leaves room for it.
   Issue #619 puzzle 2. */
sp_StrArray*sp_str_split_limit(const char*s,const char*sep,sp_int n){if(!s)sp_nil_recv("split");
  if(!sep||(sep[0]==' '&&sp_str_byte_len(sep)==1))return sp_str_split_ws_limit(s,n);
  if(n==0)return sp_str_split_drop_trailing(s,sep);
  if(n<0){sp_StrArray*a=sp_str_split(s,sep);SP_GC_ROOT(a);
    /* a non-empty String split at an empty separator ends in one empty field, as with `n` positive below */
    if(sp_str_byte_len(s)&&!sp_str_byte_len(sep))sp_str_split_push(a,"",0);return a;}
  SP_GC_ROOT_STR(s);
  SP_GC_ROOT_STR(sep);
  sp_StrArray*a=sp_StrArray_new();
  SP_GC_ROOT(a);
  const char*pe=s+sp_str_byte_len(s);
  if(pe==s)return a;
  size_t sl=sp_str_byte_len(sep);
  if(sl==0){
    const char*p=s;
    sp_int k=0;
    while(p<pe&&k<n-1){
      int cn=sp_utf8_advance(p);
      sp_str_split_push(a,p,(size_t)cn);
      p+=cn;
      k++;
    }
    if(p<pe)sp_str_split_push(a,p,(size_t)(pe-p));
    else if(!sp_str_byte_len(sep)&&p==s+sp_str_byte_len(s))sp_str_split_push(a,"",0);   /* every character is out: the limit leaves room for the empty field after them */
    return a;
  }
  const char*p=s;
  sp_int k=0;
  while(k<n-1){
    const char*f=sp_bytestr(p,(size_t)(pe-p),sep,sl);
    if(!f)break;
    size_t m=f-p;
    sp_str_split_push(a,p,m);
    p=f+sl;
    k++;
  }
  sp_str_split_push(a,p,(size_t)(pe-p));
  return a;
}
#define SP_SPLIT_WS(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)=='\f'||(c)=='\v')
/* `s.split` / `s.split(nil)` -- whitespace mode: split on runs of
   ASCII whitespace, skip leading whitespace. Issue #507: the no-arg
   form previously emitted `sp_str_split(s, 0)` and segfaulted at
   strlen(NULL). */
sp_StrArray*sp_str_split_ws(const char*s){if(!s)sp_nil_recv("split");
  SP_GC_ROOT_STR(s);
  sp_StrArray*a=sp_StrArray_new();
  SP_GC_ROOT(a);
  /* by byte length, not strlen: a NUL is an ordinary character here */
  const char*p=s,*e=s+sp_str_byte_len(s);
  while(p<e&&SP_SPLIT_WS(*p))p++;
  while(p<e){
    const char*start=p;
    while(p<e&&!SP_SPLIT_WS(*p))p++;
    size_t n=p-start;
    sp_str_split_push(a,start,n);
    while(p<e&&SP_SPLIT_WS(*p))p++;
  }
  return a;
}
/* Whitespace-mode split with an explicit limit. A single-space separator is
   special in Ruby: it skips leading whitespace and collapses separator runs,
   including when the separator comes from a variable or a limit is present.
   Positive limits preserve the unsplit remainder; negative limits retain a
   trailing empty field. */
sp_StrArray*sp_str_split_ws_limit(const char*s,sp_int n){if(!s)sp_nil_recv("split");
  if(n==0)return sp_str_split_ws(s);
  SP_GC_ROOT_STR(s);
  sp_StrArray*a=sp_StrArray_new();
  SP_GC_ROOT(a);
  const char*e=s+sp_str_byte_len(s);
  if(e==s)return a;
  if(n==1){sp_str_split_push(a,s,(size_t)(e-s));return a;}
  const char*p=s;
  while(p<e&&SP_SPLIT_WS(*p))p++;
  if(p==e){sp_str_split_push(a,p,0);return a;}
  sp_int k=0;
  while(p<e){
    const char*start=p;
    while(p<e&&!SP_SPLIT_WS(*p))p++;
    sp_str_split_push(a,start,(size_t)(p-start));
    k++;
    const char*sep_start=p;
    while(p<e&&SP_SPLIT_WS(*p))p++;
    if(p==e){if(p>sep_start&&(n<0||(n>0&&k<n)))sp_str_split_push(a,p,0);break;}
    if(n>0&&k==n-1){sp_str_split_push(a,p,(size_t)(e-p));break;}
  }
  return a;
}
#undef SP_SPLIT_WS
/* String-pattern String#scan. Regexp scans use sp_re_scan; this path handles
   a String argument, returning non-overlapping literal matches. The empty
   pattern matches at every UTF-8 character boundary, including both ends. */
sp_StrArray*sp_str_scan(const char*s,const char*pat){if(!s)sp_nil_recv("scan");
  SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pat);
  sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);
  if(!pat)sp_raise_cls("TypeError","wrong argument type nil (expected Regexp)");
  size_t pl=sp_str_byte_len(pat);const char*pe=s+sp_str_byte_len(s);
  /* `$~` is the last match, or nil when there was none */
  if(pl==0){
    const char*p=s;
    for(;;){sp_str_split_push(a,p,0);if(p>=pe)break;p+=sp_utf8_advance(p);}
    if(sp_re_track_last)sp_re_set_lit_match(s,(sp_int)(p-s),(sp_int)(p-s));
    return a;
  }
  const char*p=s,*f,*last=NULL;
  while((f=sp_bytestr(p,(size_t)(pe-p),pat,pl))!=NULL){sp_str_split_push(a,f,pl);last=f;p=f+pl;}
  if(sp_re_track_last){if(last)sp_re_set_lit_match(s,(sp_int)(last-s),(sp_int)(last-s+pl));else sp_re_clear_last_match();}
  return a;
}
/* One turn of `s.scan(pat) { }` whose block reads `$~`: sp_str_scan builds
   the rows up front, so each turn finds its own match again from byte `pos`
   and sets the registers to it (#3601). Answers where the next turn looks. */
sp_int sp_str_scan_at(const char*s,const char*pat,sp_int pos){SP_GC_ROOT_STR(s);
  size_t pl=sp_str_byte_len(pat),bl=sp_str_byte_len(s);
  const char*f=pl?sp_bytestr(s+pos,bl-(size_t)pos,pat,pl):s+pos;
  if(!f)return pos;
  sp_int at=(sp_int)(f-s);
  sp_re_set_lit_match(s,at,at+(sp_int)pl);
  if(pl)return at+(sp_int)pl;
  return f<s+bl?at+sp_utf8_advance(f):at+1;
}
/* `s.index(sub)` -- leftmost occurrence; returns a codepoint offset (not a
   byte offset), or -1 if not found. */
/* The characters from `from` up to `to` inside s, and the byte offset of
   character n: one unit per byte for a BINARY string, whatever its bytes
   spell in UTF-8, so `"\xC3\xA9\xFF".b.index("\xFF".b)` is 2 (#5535). */
static sp_int sp_str_units_between(const char *s, const char *from, const char *to) {
  if (sp_str_is_binary(s)) return (sp_int)(to - from);
  sp_int n = 0;
  while (from < to) { from += sp_utf8_advance(from); n++; }
  return n;
}
static size_t sp_str_unit_offset(const char *s, sp_int n) {
  return sp_str_is_binary(s) ? (size_t)n : sp_utf8_byte_offset(s, n);
}
sp_int sp_str_index(const char*s,const char*sub){if(!s)sp_nil_recv("index");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*f=sp_bytestr(s,sp_str_byte_len(s),sub,sp_str_byte_len(sub));if(!f)return -1;return sp_str_units_between(s,s,f);}
/* Issue #758: NULL guard + bound the start so a negative result from
   sp_str_index doesn't underflow the source pointer. */
sp_int sp_str_index_from(const char*s,const char*sub,sp_int start){if(!s)sp_nil_recv("index");sp_int cl=sp_str_length(s);if(start<0)start+=cl;if(start<0)return -1;if(start>cl)return -1;size_t boff=sp_str_unit_offset(s,start);const char*f=sp_bytestr(s+boff,sp_str_byte_len(s)-boff,sub,sp_str_byte_len(sub));if(!f)return -1;return start+sp_str_units_between(s,s+boff,f);}
/* `s.rindex(sub)` -- rightmost occurrence of sub; returns a codepoint
   offset, or -1 if not found. Empty sub matches at the end. */
sp_int sp_str_rindex(const char*s,const char*sub){if(!s)sp_nil_recv("rindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");size_t sl=sp_str_byte_len(sub);if(sl==0)return sp_str_length(s);size_t hn=sp_str_byte_len(s);const char*end=s+hn;const char*last=NULL;const char*p=s;while(p<end){const char*f=sp_bytestr(p,(size_t)(end-p),sub,sl);if(!f)break;last=f;p=f+1;}if(!last)return -1;return sp_str_units_between(s,s,last);}
/* `s.rindex(sub, pos)` -- rightmost occurrence at or before codepoint pos.
   Negative pos counts back from the char length; nil on miss. */
sp_oint sp_str_rindex_from(const char*s,const char*sub,sp_int pos){if(!s)sp_nil_recv("rindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");sp_int cl=sp_str_length(s);if(pos<0)pos=cl+pos;if(pos<0)return sp_oint_nil();size_t sl=sp_str_byte_len(sub);if(sl==0){if(pos>=cl)return sp_oint_of(cl);return sp_oint_of(pos);}size_t hn=sp_str_byte_len(s);const char*end=s+hn;const char*p=s;sp_int best=-1;const char*r=s;sp_int cur_n=0;while(p<end){const char*f=sp_bytestr(p,(size_t)(end-p),sub,sl);if(!f)break;cur_n+=sp_str_units_between(s,r,f);r=f;if(cur_n>pos)break;best=cur_n;p=f+1;}return best<0?sp_oint_nil():sp_oint_of(best);}
/* byteindex/byterindex: like index/rindex but the result and the start/pos
   arguments are BYTE offsets, not codepoint indices. Byte-oriented throughout
   (sp_bytestr), so no utf8 walk is needed. nil on miss. */
sp_oint sp_str_byteindex(const char*s,const char*sub){if(!s)sp_nil_recv("byteindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*f=sp_bytestr(s,sp_str_byte_len(s),sub,sp_str_byte_len(sub));return f?sp_oint_of((sp_int)(f-s)):sp_oint_nil();}
sp_oint sp_str_byteindex_from(const char*s,const char*sub,sp_int start){if(!s)sp_nil_recv("byteindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");sp_int bl=(sp_int)sp_str_byte_len(s);if(start<0)start+=bl;if(start<0||start>bl)return sp_oint_nil();const char*f=sp_bytestr(s+start,(size_t)(bl-start),sub,sp_str_byte_len(sub));return f?sp_oint_of((sp_int)(f-s)):sp_oint_nil();}
sp_oint sp_str_byterindex(const char*s,const char*sub){if(!s)sp_nil_recv("byterindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");size_t sl=sp_str_byte_len(sub);size_t hn=sp_str_byte_len(s);if(sl==0)return sp_oint_of((sp_int)hn);const char*end=s+hn;const char*last=NULL;const char*p=s;while(p<end){const char*f=sp_bytestr(p,(size_t)(end-p),sub,sl);if(!f)break;last=f;p=f+1;}return last?sp_oint_of((sp_int)(last-s)):sp_oint_nil();}
sp_oint sp_str_byterindex_from(const char*s,const char*sub,sp_int pos){if(!s)sp_nil_recv("byterindex");if(!sub)sp_raise_cls("TypeError","no implicit conversion of nil into String");sp_int bl=(sp_int)sp_str_byte_len(s);if(pos<0)pos+=bl;if(pos<0)return sp_oint_nil();if(pos>bl)pos=bl;size_t sl=sp_str_byte_len(sub);if(sl==0)return sp_oint_of(pos);const char*end=s+bl;const char*p=s;sp_int best=-1;while(p<end){const char*f=sp_bytestr(p,(size_t)(end-p),sub,sl);if(!f)break;sp_int off=(sp_int)(f-s);if(off>pos)break;best=off;p=f+1;}return best<0?sp_oint_nil():sp_oint_of(best);}
/* start/len are codepoint indices/counts. */
/* `s[start, len]` char-indexed slice (UTF-8 aware). Negative start counts
   back from the char length. CRuby's nil contract: start past the length
   (or before -length) and a negative len return NULL; start == length and
   len == 0 return "". A single-char ASCII result aliases the per-process
   sp_char_cache so common indexing avoids an allocation. */
const char*sp_str_sub_range(const char*s,sp_int start,sp_int len){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("[]");size_t _fb;int fixed=sp_str_fixed_width(s,&_fb);sp_int cl=fixed?(sp_int)_fb:sp_str_length(s);if(start<0)start+=cl;if(start<0||start>cl||len<0)return NULL;if(start==cl||len==0){return sp_str_is_binary(s)?sp_str_empty_binary():&("\xff" "")[1];}if(len>cl-start)len=cl-start;int bin=fixed?sp_str_is_binary(s):0;size_t boff=fixed?(size_t)start:sp_utf8_byte_offset(s,start);size_t blen_total=fixed?_fb:sp_str_byte_len(s);size_t bp=boff;sp_int rem=len;while(rem>0&&bp<blen_total){bp+=fixed?1:sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(bin&&len==1&&blen==1)return sp_bin_char((unsigned char)s[boff]);if(!bin&&len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(c!=0){if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;sp_str_set_len(r,blen);if(bin)sp_str_mark_binary(r);return r;}
/* Single-character `s[i]`. Returns NULL on out-of-bounds so the caller can
   yield CRuby's `"hello"[20] -> nil`. */
const char*sp_str_char_at_or_nil(const char*s,sp_int i){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("[]");sp_int cl=sp_str_length(s);if(i<0)i+=cl;if(i<0||i>=cl)return NULL;return sp_str_sub_range(s,i,1);}
const char*sp_str_sub_range_len(const char*s,sp_int cl,sp_int start,sp_int len){SP_GC_ROOT_STR(s);if(start<0)start+=cl;if(start<0||start>cl||len<0)return NULL;if(start==cl||len==0){return sp_str_is_binary(s)?sp_str_empty_binary():&("\xff" "")[1];}if(len>cl-start)len=cl-start;size_t _fb;int fixed=sp_str_fixed_width(s,&_fb);int bin=fixed?sp_str_is_binary(s):0;size_t boff=fixed?(size_t)start:sp_utf8_byte_offset(s,start);size_t blen_total=fixed?_fb:sp_str_byte_len(s);size_t bp=boff;sp_int rem=len;while(rem>0&&bp<blen_total){bp+=fixed?1:sp_utf8_advance(s+bp);rem--;}if(bp>blen_total)bp=blen_total;size_t bend=bp;size_t blen=bend-boff;if(bin&&len==1&&blen==1)return sp_bin_char((unsigned char)s[boff]);if(!bin&&len==1&&blen==1){unsigned char c=(unsigned char)s[boff];if(c!=0){if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}}char*r=sp_str_alloc_raw(blen+1);memcpy(r,s+boff,blen);r[blen]=0;sp_str_set_len(r,blen);if(bin)sp_str_mark_binary(r);return r;}
const char*sp_str_sub_range_r(const char*s,sp_int start,sp_int end_,sp_int excl){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("[]");sp_int cl=sp_str_length(s);if(end_<0)end_+=cl;if(start<0)start+=cl;if(start<0)return NULL;if(end_>=cl){end_=cl;excl=1;}sp_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
const char*sp_str_sub_range_len_r(const char*s,sp_int cl,sp_int start,sp_int end_,sp_int excl){SP_GC_ROOT_STR(s);if(end_<0)end_+=cl;if(start<0)start+=cl;if(start<0)return NULL;if(end_>=cl){end_=cl;excl=1;}sp_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_str_sub_range_len(s,cl,start,n);}
/* walks by INDEX to the header length: `while(*p)` stopped at an embedded NUL
   and reversed only the prefix */
const char*sp_str_reverse(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("reverse");size_t bl=sp_str_byte_len(s);char*r=sp_str_alloc_raw(bl+1);size_t end=bl;for(size_t i=0;i<bl;){int cn=sp_utf8_advance(s+i);if(cn<1)cn=1;if((size_t)cn>bl-i)cn=(int)(bl-i);end-=(size_t)cn;memcpy(r+end,s+i,(size_t)cn);i+=(size_t)cn;}r[bl]=0;sp_str_set_len(r,bl);return sp_str_bin_like(s,r);}
sp_int sp_str_count(const char*s,const char*chars){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(chars);if(!chars)sp_raise_cls("TypeError","no implicit conversion of nil into String");int negate=0;const char*csp=chars;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t setn;uint32_t*set=sp_utf8_decode_charset_n(csp,sp_str_byte_len(chars)-(size_t)(csp-chars),&setn);sp_int c=0;const char*p=s;const char*end=s+sp_str_byte_len(s);while(p<end){uint32_t cp;p+=sp_utf8_decode(p,&cp);int in_set=sp_utf8_set_has(set,setn,cp);if(negate)in_set=!in_set;if(in_set)c++;}free(set);return c;}
sp_int sp_str_count_n(const char*s,const char**chars,sp_int n){SP_GC_ROOT_STR(s);if(n<=0)return 0;size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(sp_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset_n(cs,sp_str_byte_len(chars[i])-(size_t)(cs-chars[i]),&setns[i]);}sp_int c=0;const char*p=s;const char*end=s+sp_str_byte_len(s);while(p<end){uint32_t cp;p+=sp_utf8_decode(p,&cp);int all=1;for(sp_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(all)c++;}for(sp_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);return c;}
/* The codepoints walked to the recorded byte length, as sp_str_chars walks:
   a BINARY String's are its bytes, and a NUL is a character like any other.
   String#codepoints with a block yields these. */
sp_IntArray*sp_str_codepoints_all(const char*s){SP_GC_ROOT_STR(s);sp_IntArray*a=sp_IntArray_new();if(!s)sp_nil_recv("codepoints");SP_GC_ROOT(a);int bin=sp_str_is_binary(s);const char*end=s+sp_str_byte_len(s);const char*p=s;while(p<end){uint32_t cp;int n=1;if(bin)cp=(unsigned char)*p;else n=sp_utf8_decode(p,&cp);if(p+n>end)n=(int)(end-p);sp_IntArray_push(a,(sp_int)cp);p+=n;}return a;}
sp_IntArray*sp_str_codepoints(const char*s){SP_GC_ROOT_STR(s);sp_IntArray*a=sp_IntArray_new();if(!s)sp_nil_recv("codepoints");const char*p=s;while(*p){uint32_t cp;int n=sp_utf8_decode(p,&cp);sp_IntArray_push(a,(sp_int)cp);p+=n;}return a;}
/* walk to the recorded byte length rather than to the first NUL: a NUL is an
   ordinary one-byte character and the characters after it are real (#3473) */
sp_StrArray*sp_str_chars(const char*s){SP_GC_ROOT_STR(s);sp_StrArray*a=sp_StrArray_new();if(!s)sp_nil_recv("chars");SP_GC_ROOT(a);int bin=sp_str_is_binary(s);const char*end=s+sp_str_byte_len(s);const char*p=s;while(p<end){int n=bin?1:sp_utf8_advance(p);if(p+n>end)n=(int)(end-p);char*c=sp_str_alloc(n);memcpy(c,p,n);c[n]=0;sp_StrArray_push(a,c);p+=n;}return a;}
/* Issue #798: guard NULL inputs (CRuby treats nil/no-op gracefully). */
const char*sp_str_tr(const char*s,const char*from,const char*to){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(from);SP_GC_ROOT_STR(to);if(!s)sp_nil_recv("tr");if(!from||!to)return s;int negate=0;const char*fp=from;if(*fp=='^'&&*(fp+1)){negate=1;fp++;}size_t fn,tn;uint32_t*fcps=sp_utf8_decode_charset_n(fp,sp_str_byte_len(from)-(size_t)(fp-from),&fn);uint32_t*tcps=sp_utf8_decode_charset_n(to,sp_str_byte_len(to),&tn);size_t bl=sp_str_byte_len(s);size_t cap=((bl*4))+1;char*buf=(char*)malloc(cap);size_t n=0;const char*p=s,*pe=s+bl;int bin=sp_str_is_binary(s);while(p<pe){uint32_t cp;int cn=bin?(cp=(unsigned char)*p,1):sp_utf8_decode(p,&cp);size_t mi=fn;for(size_t j=0;j<fn;j++)if(fcps[j]==cp){mi=j;break;}int in_set=(mi<fn);if(negate)in_set=!in_set;if(in_set&&tn>0){uint32_t rep=negate?tcps[tn-1]:(mi<tn?tcps[mi]:tcps[tn-1]);if(bin)buf[n++]=(char)rep;else n+=sp_utf8_encode(rep,buf+n);}
else if(in_set){}
else{memcpy(buf+n,p,cn);n+=cn;}p+=cn;}buf[n]=0;char*r=sp_str_alloc(n);memcpy(r,buf,n+1);free(buf);free(fcps);free(tcps);return sp_str_bin_like(s,r);}
const char*sp_str_tr_s(const char*s,const char*from,const char*to){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(from);SP_GC_ROOT_STR(to);
  if(!s)sp_nil_recv("tr_s");
  if(!from||!to)return s;
  int negate=0;const char*fp=from;
  if(*fp=='^'&&*(fp+1)){negate=1;fp++;}
  size_t fn,tn;
  uint32_t*fcps=sp_utf8_decode_charset_n(fp,sp_str_byte_len(from)-(size_t)(fp-from),&fn);
  uint32_t*tcps=sp_utf8_decode_charset_n(to,sp_str_byte_len(to),&tn);
  size_t bl=sp_str_byte_len(s);
  size_t cap=(((bl*4)))+1;
  char*buf=(char*)malloc(cap);
  size_t n=0;
  const char*p=s,*pe=s+bl;int bin=sp_str_is_binary(s);
  uint32_t last_emit=0; int has_last=0; int last_was_translated=0;
  while(p<pe){
    uint32_t cp; int cn=bin?(cp=(unsigned char)*p,1):sp_utf8_decode(p,&cp);
    size_t mi=fn;
    for(size_t j=0;j<fn;j++)if(fcps[j]==cp){mi=j;break;}
    int in_set=(mi<fn);
    if(negate)in_set=!in_set;
    uint32_t emit_cp;
    int translated=0;
    if(in_set){
      if(tn>0){
        emit_cp=negate?tcps[tn-1]:(mi<tn?tcps[mi]:tcps[tn-1]);
        translated=1;
      }
else {
        p+=cn; continue;
      }
    }
else {
      emit_cp=cp;
      translated=0;
    }
    /* Squeeze only when both the previous and current emit were
       translated, AND the emitted codepoints match. */
    if(has_last && last_was_translated && translated && last_emit==emit_cp){
      /* skip */
    }
else {
      if(translated&&bin)buf[n++]=(char)emit_cp;else if(translated)n+=sp_utf8_encode(emit_cp,buf+n);else{memcpy(buf+n,p,(size_t)cn);n+=(size_t)cn;}
      last_emit=emit_cp;
      has_last=1;
      last_was_translated=translated;
    }
    p+=cn;
  }
  buf[n]=0;
  char*r=sp_str_alloc(n);
  memcpy(r,buf,n+1);
  free(buf); free(fcps); free(tcps);
  return sp_str_bin_like(s,r);
}
const char*sp_str_delete(const char*s,const char*chars){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(chars);if(!s)sp_nil_recv("delete");if(!chars)return s;int negate=0;const char*csp=chars;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t setn;uint32_t*set=sp_utf8_decode_charset_n(csp,sp_str_byte_len(chars)-(size_t)(csp-chars),&setn);size_t bl=sp_str_byte_len(s);char*buf=(char*)malloc(bl+1);if(!buf)sp_oom_die();size_t n=0;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int in_set=sp_utf8_set_has(set,setn,cp);if(negate)in_set=!in_set;if(!in_set){memcpy(buf+n,p,cn);n+=cn;}p+=cn;}buf[n]=0;free(set);char*r=sp_str_alloc(n);memcpy(r,buf,n+1);free(buf);return sp_str_bin_like(s,r);}
const char*sp_str_squeeze(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("squeeze");size_t bl=sp_str_byte_len(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;uint32_t prev=0xFFFFFFFFu;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);if(cp!=prev){memcpy(r+n,p,cn);n+=cn;prev=cp;}p+=cn;}r[n]=0;sp_str_set_len(r,n);return sp_str_bin_like(s,r);}
const char*sp_str_squeeze_chars(const char*s,const char*cs){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(cs);if(!s)sp_nil_recv("squeeze");if(!cs)return sp_str_squeeze(s);int negate=0;const char*csp=cs;if(*csp=='^'&&*(csp+1)){negate=1;csp++;}size_t fn;uint32_t*fcps=sp_utf8_decode_charset_n(csp,sp_str_byte_len(cs)-(size_t)(csp-cs),&fn);size_t bl=sp_str_byte_len(s);char*r=sp_str_alloc_raw(bl+1);size_t n=0;uint32_t prev=0xFFFFFFFFu;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int in_set=0;for(size_t j=0;j<fn;j++)if(fcps[j]==cp){in_set=1;break;}if(negate)in_set=!in_set;if(!in_set){memcpy(r+n,p,cn);n+=cn;prev=0xFFFFFFFFu;}
else if(cp!=prev){memcpy(r+n,p,cn);n+=cn;prev=cp;}p+=cn;}r[n]=0;sp_str_set_len(r,n);free(fcps);return sp_str_bin_like(s,r);}
const char*sp_str_delete_n(const char*s,const char**chars,sp_int n){SP_GC_ROOT_STR(s);if(!s)return sp_str_empty;if(n<=0)return s;size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(sp_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset_n(cs,sp_str_byte_len(chars[i])-(size_t)(cs-chars[i]),&setns[i]);}size_t bl=sp_str_byte_len(s);char*buf=(char*)malloc(bl+1);if(!buf)sp_oom_die();size_t m=0;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int all=1;for(sp_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(!all){memcpy(buf+m,p,cn);m+=cn;}p+=cn;}buf[m]=0;for(sp_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);char*r=sp_str_alloc(m);memcpy(r,buf,m+1);free(buf);return sp_str_bin_like(s,r);}
const char*sp_str_squeeze_n(const char*s,const char**chars,sp_int n){SP_GC_ROOT_STR(s);if(!s)return sp_str_empty;if(n<=0)return sp_str_squeeze(s);size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(sp_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset_n(cs,sp_str_byte_len(chars[i])-(size_t)(cs-chars[i]),&setns[i]);}size_t bl=sp_str_byte_len(s);char*r=sp_str_alloc_raw(bl+1);size_t m=0;uint32_t prev=0xFFFFFFFFu;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int all=1;for(sp_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(!all){memcpy(r+m,p,cn);m+=cn;prev=0xFFFFFFFFu;}
else if(cp!=prev){memcpy(r+m,p,cn);m+=cn;prev=cp;}p+=cn;}r[m]=0;sp_str_set_len(r,m);for(sp_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);return sp_str_bin_like(s,r);}
static const char*sp_str_strip_sets(const char*s,const char**chars,sp_int n,int mode){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv(mode==1?"lstrip":mode==2?"rstrip":"strip");size_t*setns=(size_t*)malloc(n*sizeof(size_t));uint32_t**sets=(uint32_t**)malloc(n*sizeof(uint32_t*));int*negs=(int*)malloc(n*sizeof(int));for(sp_int i=0;i<n;i++){if(!chars[i])sp_raise_cls("TypeError","no implicit conversion of nil into String");const char*cs=chars[i];negs[i]=0;if(*cs=='^'&&*(cs+1)){negs[i]=1;cs++;}sets[i]=sp_utf8_decode_charset_n(cs,sp_str_byte_len(chars[i])-(size_t)(cs-chars[i]),&setns[i]);}size_t bl=sp_str_byte_len(s),a=bl,e=0;const char*p=s,*pe=s+bl;while(p<pe){uint32_t cp;int cn=sp_utf8_decode(p,&cp);int all=1;for(sp_int i=0;i<n;i++){int in_set=sp_utf8_set_has(sets[i],setns[i],cp);if(negs[i])in_set=!in_set;if(!in_set){all=0;break;}}if(!all){if(a==bl)a=(size_t)(p-s);e=(size_t)(p-s)+cn;}p+=cn;}for(sp_int i=0;i<n;i++)free(sets[i]);free(sets);free(setns);free(negs);if(mode==1)e=bl;else if(mode==2)a=0;if(e<a)e=a;size_t m=e-a;char*r=sp_str_alloc(m);memcpy(r,s+a,m);r[m]=0;if(sp_str_is_binary(s))sp_str_mark_binary(r);return r;}
const char*sp_str_strip_n(const char*s,const char**chars,sp_int n){return sp_str_strip_sets(s,chars,n,3);}
const char*sp_str_lstrip_n(const char*s,const char**chars,sp_int n){return sp_str_strip_sets(s,chars,n,1);}
const char*sp_str_rstrip_n(const char*s,const char**chars,sp_int n){return sp_str_strip_sets(s,chars,n,2);}
/* String#scrub!: replace invalid bytes IN PLACE. CRuby raises FrozenError on
   a frozen receiver only when it would actually change it -- a frozen string
   with nothing to scrub comes back unchanged (#3338). */
const char *sp_str_scrub_bang(const char *s, const char *repl) {
  SP_GC_ROOT_STR(s); SP_GC_ROOT_STR(repl);
  if (!s) return s;
  const char *r = sp_str_scrub(s, repl);
  if (r == s || sp_str_eq(r, s)) return s;
  sp_str_check_mutable(s);
  return r;
}
/* The length of the UTF-8 character at p (n bytes left), or of the invalid
   sequence there, *bad set: a lead byte and the continuation bytes that fit
   it before one that does not, as CRuby takes them (Unicode's maximal
   subpart), so a truncated "\xE3\x81" is one sequence and an overlong
   "\xC0\xAF" or a surrogate "\xED\xA0\x80" is one per byte. */
static size_t sp_utf8_scan1(const unsigned char *p, size_t n, int *bad) {
  unsigned char c = p[0], lo = 0x80, hi = 0xBF;
  int need;
  *bad = 0;
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) need = 1;
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
  else { *bad = 1; return 1; }
  size_t k = 1;
  for (; k <= (size_t)need; k++) {
    if (k >= n || p[k] < lo || p[k] > hi) { *bad = 1; return k; }
    lo = 0x80; hi = 0xBF;
  }
  return k;
}
/* String#scrub's walk: the offset of the first invalid sequence of s (bl
   bytes) at or after `from`, its length in *len; bl when there is none. */
sp_int sp_str_scrub_bad(const char *s, sp_int bl, sp_int from, sp_int *len) {
  sp_int i = from;
  while (i < bl) {
    int bad;
    size_t k = sp_utf8_scan1((const unsigned char *)s + i, (size_t)(bl - i), &bad);
    if (bad) { *len = (sp_int)k; return i; }
    i += (sp_int)k;
  }
  *len = 0;
  return bl;
}
/* A replacement scrub's block answered: valid UTF-8, or CRuby's
   ArgumentError naming it. */
const char *sp_str_scrub_repl(const char *r) {
  sp_int len, bl = (sp_int)sp_str_byte_len(r);
  if (sp_str_scrub_bad(r, bl, 0, &len) < bl)
    sp_raise_cls("ArgumentError", sp_sprintf("replacement must be valid byte sequence '%s'", sp_str_inspect(r)));
  return r;
}
/* String#scrub: an ASCII-8BIT receiver has no invalid sequence, so CRuby
   answers a copy of it, still ASCII-8BIT, without looking at the
   replacement; any other receiver is walked as UTF-8 (sp_str_scrub_utf8). */
const char *sp_str_scrub(const char *s, const char *repl) {
  if (s && sp_str_is_binary(s)) return sp_str_dup(s);
  return sp_str_scrub_utf8(s, repl);
}
/* each invalid UTF-8 sequence (sp_str_scrub_bad) is replaced; a NULL
   replacement is U+FFFD (3 UTF-8 bytes: EF BF BD), as in CRuby. encode's
   UTF-8 to UTF-8 conversion calls it whatever the receiver's flag. */
const char *sp_str_scrub_utf8(const char *s, const char *repl) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(repl);
  if(!s)sp_nil_recv("scrub");
  static const char fffd[] = "\xEF\xBF\xBD";
  const char *r = repl ? repl : fffd;
  size_t rlen = repl ? sp_str_byte_len(repl) : strlen(r);
  sp_int bl = (sp_int)sp_str_byte_len(s);
  size_t cap = (size_t)bl + 64;
 /* malloc scratch (grown with realloc on invalid-byte runs); the final
    string is emitted at the exact length below. */
  char *out = (char *)malloc(cap);
  size_t olen = 0;
  sp_int i = 0;
  while (i < bl) {
    sp_int len, at = sp_str_scrub_bad(s, bl, i, &len);
    if (at < bl && repl && i == 0) sp_str_scrub_repl(repl);   /* checked once, where it is used */
    size_t keep = (size_t)(at - i), add = at < bl ? rlen : 0;
    if (olen + keep + add + 1 >= cap) { cap = (olen + keep + add) * 2 + 64; out = (char *)realloc(out, cap); }
    memcpy(out + olen, s + i, keep);
    olen += keep;
    memcpy(out + olen, r, add);
    olen += add;
    i = at + len;
    if (at >= bl) break;
  }
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
const char*sp_str_ljust(const char*s,sp_int w){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("ljust");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);const char*e=sp_justify_size_err(bl,(uint64_t)(w-cl));if(e)sp_justify_size_raise(e);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memcpy(r,s,bl);memset(r+bl,' ',pad);r[bl+pad]=0;sp_str_set_len(r,bl+pad);return sp_str_bin_like(s,r);}
const char*sp_str_rjust(const char*s,sp_int w){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("rjust");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);const char*e=sp_justify_size_err(bl,(uint64_t)(w-cl));if(e)sp_justify_size_raise(e);size_t pad=(size_t)(w-cl);char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',pad);memcpy(r+pad,s,bl);r[bl+pad]=0;sp_str_set_len(r,bl+pad);return sp_str_bin_like(s,r);}
const char*sp_str_center(const char*s,sp_int w){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("center");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);const char*e=sp_justify_size_err(bl,(uint64_t)(w-cl));if(e)sp_justify_size_raise(e);sp_int pad=w-cl;sp_int left=pad/2;sp_int right=pad-left;char*r=sp_str_alloc_raw(bl+pad+1);memset(r,' ',left);memcpy(r+left,s,bl);memset(r+left+bl,' ',right);r[bl+pad]=0;sp_str_set_len(r,bl+(size_t)pad);return sp_str_bin_like(s,r);}
const char*sp_str_ljust2(const char*s,sp_int w,const char*pad){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pad);if(!s)sp_nil_recv("ljust");/* an empty pad is rejected whatever the width, before the no-op case */if(!pad||sp_str_byte_len(pad)==0)sp_raise_cls("ArgumentError","zero width padding");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);sp_str_set_len(r,bl);return r;}sp_int need=w-cl;const char*e=sp_justify_size_err(bl,sp_pad_bytes(pcps,pn,(uint64_t)need));if(e){free(pcps);sp_justify_size_raise(e);}size_t padb=(size_t)sp_pad_bytes(pcps,pn,(uint64_t)need);char*r=sp_str_alloc_raw(bl+padb+1);memcpy(r,s,bl);size_t n=bl;for(sp_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;sp_str_set_len(r,n);free(pcps);return r;}
const char*sp_str_rjust2(const char*s,sp_int w,const char*pad){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pad);if(!s)sp_nil_recv("rjust");/* an empty pad is rejected whatever the width, before the no-op case */if(!pad||sp_str_byte_len(pad)==0)sp_raise_cls("ArgumentError","zero width padding");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);sp_str_set_len(r,bl);return r;}sp_int need=w-cl;const char*e=sp_justify_size_err(bl,sp_pad_bytes(pcps,pn,(uint64_t)need));if(e){free(pcps);sp_justify_size_raise(e);}size_t padb=(size_t)sp_pad_bytes(pcps,pn,(uint64_t)need);char*r=sp_str_alloc_raw(bl+padb+1);size_t n=0;for(sp_int i=0;i<need;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);r[n+bl]=0;sp_str_set_len(r,n+bl);free(pcps);return r;}
const char*sp_str_center2(const char*s,sp_int w,const char*pad){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pad);if(!s)sp_nil_recv("center");/* an empty pad is rejected whatever the width, before the no-op case */if(!pad||sp_str_byte_len(pad)==0)sp_raise_cls("ArgumentError","zero width padding");sp_int cl=sp_str_length(s);if(cl>=w)return sp_str_dup(s);size_t bl=sp_str_byte_len(s);size_t pn;uint32_t*pcps=sp_utf8_decode_all(pad,&pn);if(pn==0){free(pcps);char*r=sp_str_alloc_raw(bl+1);memcpy(r,s,bl+1);sp_str_set_len(r,bl);return r;}sp_int pd=w-cl;sp_int left=pd/2;sp_int right=pd-left;uint64_t lb64=sp_pad_bytes(pcps,pn,(uint64_t)left),rb64=sp_pad_bytes(pcps,pn,(uint64_t)right);const char*e=sp_justify_size_err(bl,(lb64==UINT64_MAX||rb64==UINT64_MAX||lb64>UINT64_MAX-rb64)?UINT64_MAX:lb64+rb64);if(e){free(pcps);sp_justify_size_raise(e);}size_t leftb=(size_t)lb64,rightb=(size_t)rb64;char*r=sp_str_alloc_raw(leftb+bl+rightb+1);size_t n=0;for(sp_int i=0;i<left;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);memcpy(r+n,s,bl);n+=bl;for(sp_int i=0;i<right;i++)n+=sp_utf8_encode(pcps[i%pn],r+n);r[n]=0;sp_str_set_len(r,n);free(pcps);return r;}
sp_oint sp_str_index_opt(const char *s, const char *sub)                         {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_index(s, sub);              return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
sp_oint sp_str_index_from_opt(const char *s, const char *sub, sp_int start)     {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_index_from(s, sub, start);  return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
sp_oint sp_str_rindex_opt(const char *s, const char *sub)                        {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_rindex(s, sub);             return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
