/* sp_string.c -- the cold sp_String in-place mutators (see sp_string.h).
   prepend / insert / replace / dup are off the hot string-building path, so they
   are compiled once here instead of inline in every generated TU. */
#include "sp_string.h"
#include "sp_str.h"   /* sp_str_concat (sp_str_keep_copy) */
#include <string.h>

void sp_String_prepend(sp_String*s,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("prepend"); if (!t)return;if(sp_String_is_immutable(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(!sp_fd_grow(s,s->len+tl))return;memmove(s->data+tl,s->data,s->len+1);memcpy(s->data,t,tl);s->len+=tl;sp_fd_publish(s);}
/* String#insert(idx, str): insert at idx; negative idx is relative to len+1. */
void sp_String_insert(sp_String*s,int64_t idx,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("insert"); if (!t)return;if(sp_String_is_immutable(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(tl==0)return;if(idx<0)idx+=s->len+1;if(idx<0)idx=0;if(idx>s->len)idx=s->len;if(!sp_fd_grow(s,s->len+tl))return;memmove(s->data+idx+tl,s->data+idx,s->len-idx+1);memcpy(s->data+idx,t,tl);s->len+=tl;sp_fd_publish(s);}
/* Whether the n bytes at p are all below 0x80. */
static int sp_bytes_ascii(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) if ((unsigned char)p[i] >= 0x80) return 0;
  return 1;
}
/* The in-place splice: bytes [b, e) of s replaced by the vl bytes at v,
   where s lives. The tail moves and nothing else is copied; a splice into a
   new String written back (sp_String_set_bin) copied the whole receiver
   twice, so a loop of splices was quadratic in its length. `bin` is the
   encoding the result takes: 1 BINARY, 0 text, -1 the receiver's own. The
   caller has made every check but this one: FrozenError, after them, where
   sp_String_set_bin raised it after the splice it was handed. v may be a
   view of s's own bytes (`s[0] = s`), and is copied first.

   SP_STR_SIZE_ASCII7 records that every byte is ASCII. A splice preserves
   that proof only when the replacement and affected bytes are ASCII. An
   arbitrary splice leaves the next reader to check the resulting bytes. */
static void sp_String_splice_in(sp_String *s, size_t b, size_t e, const char *v, size_t vl, int bin) {
  if (sp_String_is_immutable(s)) { sp_raise_frozen_str(s->data); return; }
  char *own = NULL;
  if ((uintptr_t)v >= (uintptr_t)s->data && (uintptr_t)v <= (uintptr_t)(s->data + s->len)) {
    own = (char *)malloc(vl + 1);
    if (!own) sp_oom_die();
    memcpy(own, v, vl);
    v = own;
  }
  size_t bl = (size_t)s->len, nl = bl - (e - b) + vl;
  const unsigned char *o = (const unsigned char *)s->data;
  int ascii7 = sp_str_is_ascii7(s->data) && sp_bytes_ascii(v, vl) && sp_bytes_ascii(s->data + b, e - b) &&
               (b == 0 || o[b - 1] < 0x80) && (e == bl || o[e] < 0x80);
  if (!sp_fd_grow(s, (int64_t)nl)) { free(own); return; }
  char *d = s->data;
  memmove(d + b + vl, d + e, bl - e);
  memcpy(d + b, v, vl);
  s->len = (int64_t)nl; d[nl] = 0;
  free(own);
  if (bin == 1) s->binary = 1;
  else if (bin == 0 && s->binary) { s->binary = 0; sp_str_as_text(d); }
  sp_fd_publish(s);
  if (ascii7) sp_str_mark_ascii7(d);
}
/* s[from, n] = val by characters, in place: String#[]= of an index, a start
   and a length, a Range or a substring, and String#insert. It checks the
   length and start before splicing. A String store can use byte indexing
   chosen before the replacement's conversion changes the receiver.
   The result's encoding is the one its pieces joined in: the receiver's, unless
   val has high bytes in the other encoding, which the result takes where
   the receiver's kept bytes are all ASCII and is BINARY where they are not.
   keep_enc leaves the receiver's encoding as it is, as sp_String_set_bin did.

   Neither splice roots s or val: nothing in them allocates from the
   collector before the bytes are written (growth and a moved payload's
   retirement are malloc and realloc), so neither can be collected under
   them, and a raise from a check leaves no root of theirs behind (an inline
   rescue does not restore sp_gc_nroots). */
void sp_String_splice_chars(sp_String *s, sp_int from, sp_int n, const char *val, int single_byte, int keep_enc) {
  if (SP_UNLIKELY(!s)) sp_nil_recv("[]=");
  /* (an empty String with the marker byte the readers look behind: a bare
     "" has none, and sp_str_byte_len reads the byte before it) */
  if (!val) val = sp_str_empty;
  const char *d = s->data;
  if (n < 0) { sp_raise_cls("IndexError", sp_sprintf("negative length %lld", (long long)n)); return; }
  int bin = keep_enc ? -1 : sp_str_splice_encoding(d, val);
  sp_int len = (sp_int)sp_str_length(d);
  sp_int from0 = from;
  if (from < 0) from += len;
  if (from < 0 || from > len) {
    sp_raise_cls("IndexError", sp_sprintf("index %lld out of string", (long long)from0));
    return;
  }
  if (n > len - from) n = len - from;
  size_t bl = sp_str_byte_len(d), b, e, fb;
  if (single_byte || sp_str_fixed_width(d, &fb) || sp_str_is_binary(d)) { b = (size_t)from; e = b + (size_t)n; }
  else {
    b = sp_utf8_byte_offset(d, from);
    e = b;
    for (sp_int k = n; k > 0 && e < bl; k--) e += sp_utf8_unit_len(d + e, bl - e);
    if (e > bl) e = bl;
  }
  sp_String_splice_in(s, b, e, val, sp_str_byte_len(val), bin);
}
/* String#bytesplice(start, len, val, vstart, vlen) in place: bytes
   start...start+len of s replaced by bytes vstart...vstart+vlen of val,
   checked as sp_str_bytesplice5 checks them (sp_str_bytesplice_check), and
   the result in the encoding the two are compatible in, or with keep_enc the
   receiver's own. */
void sp_String_splice_bytes(sp_String *s, sp_int start, sp_int len, const char *val, sp_int vstart, sp_int vlen, int keep_enc) {
  if (SP_UNLIKELY(!s)) sp_nil_recv("bytesplice");
  if (!val) val = &("\xff" "")[1];
  int bin = sp_str_bytesplice_check(s->data, &start, &len, val, &vstart, &vlen);
  sp_String_splice_in(s, (size_t)start, (size_t)(start + len), val + vstart, (size_t)vlen, keep_enc ? -1 : bin);
}
/* A match supplies a byte span in the pre-conversion String. The replacement
   may have changed the String, including the character boundaries at that span. */
void sp_String_splice_match(sp_String *s, sp_int start, sp_int len, const char *val) {
  if (!s) sp_nil_recv("[]=");
  const char *bytes = sp_String_cstr(s);
  if (!val) val = sp_str_empty;
  int bin = sp_str_splice_encoding(bytes, val);
  if (sp_String_is_immutable(s)) sp_raise_frozen_str(bytes);
  size_t begin = (size_t)start, count = (size_t)len, length = (size_t)s->len;
  if (begin > length || (begin == length && count > 0)) return;
  size_t replacement = sp_str_byte_len(val);
  if (count > length - begin) {
    if (replacement == count) replacement = length - begin;
    count = length - begin;
  }
  sp_String_splice_in(s, begin, begin + count, val, replacement, bin);
}
/* String#replace(s): replace entire content. */
void sp_String_replace(sp_String*s,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("replace"); if (!t)return;if(sp_String_is_immutable(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(!sp_fd_grow(s,tl))return;memcpy(s->data,t,tl);s->data[tl]='\0';s->len=tl;sp_fd_publish(s);}
sp_String*sp_String_dup(sp_String*s){SP_GC_ROOT(s);return sp_String_new(s->data);}
/* The first growth past an inline payload (sp_String_new_inline_len) moves
   it to a malloc'd block, which the object now owns: its bytes are counted
   and the finalizer that frees them installed, as sp_String_new_len does
   from the start. The inline room stays part of the object. Rare, and kept
   out of sp_fd_grow so that one still inlines into every append. */
int sp_fd_grow_inline(sp_String *s, int64_t need){
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)s - sizeof(sp_gc_hdr));
  int64_t new_cap = (need * 2) + 16;
  sp_str_lcache_drop(s->data);
  char *raw = (char *)malloc(SP_FD_OVH + new_cap);
  if (!raw) return 0;
  char *data = sp_fd_setup(raw);
  /* the whole old room, as the realloc of an outside payload keeps it: a
     caller may have set len short of bytes it still reads (set_bin of the
     String's own bytes empties it, then appends them) */
  memcpy(data, s->data, (size_t)(s->cap > s->len ? s->cap : s->len) + 1);
  s->cap = new_cap; s->data = data; sp_fd_own(s);
  h->size += s->cap + SP_FD_OVH; sp_gc_bytes_add(s->cap + SP_FD_OVH);
  if (!h->finalize) { h->finalize = sp_String_fin; sp_slab_set_fin(h); }
  return 1;
}

/* The cached spelling for Symbol#name and internal byte readers. The symbol
   table's name has no frozen header of its own, so every symbol has one
   frozen copy (0xf1, sp_sym_str_tag), built on first use and retained as long
   as the symbol. Its header holds the length, hash and encoding flags. The
   public to_s and id2name operations allocate their own String handles. */
typedef struct { sp_str_hdr h; unsigned char m; char d[]; } sp_sym_str_obj;
static const char**sp_sym_str_tab=NULL;
static sp_int sp_sym_str_cap=0;
const char*sp_sym_to_s_frozen(sp_sym id){
  const char*nm=sp_sym_name_fn?sp_sym_name_fn(id):sp_str_empty;
  if(id<0||!sp_sym_name_fn)return nm;
  SP_HEAP_LOCK();
  const char*hit=id<sp_sym_str_cap?sp_sym_str_tab[id]:NULL;
  SP_HEAP_UNLOCK();
  if(hit)return hit;
  size_t n=sp_str_byte_len(nm);
  sp_sym_str_obj*o=(sp_sym_str_obj*)malloc(sizeof(sp_sym_str_obj)+n+1);
  if(!o)return nm;
  int ascii7=1;
  for(size_t i=0;i<n;i++)if((unsigned char)nm[i]>=0x80)ascii7=0;
  o->h.next=(sp_str_hdr*)&sp_sym_str_tag;
  o->h.size=(uint32_t)(n+1)|(ascii7?SP_STR_SIZE_ASCII7:0u)|(sp_str_is_binary(nm)?SP_STR_SIZE_BINARY:0u);
  o->h.len=(uint32_t)n;o->h.hash=0;
  o->m=0xf1;memcpy(o->d,nm,n);o->d[n]=0;
  SP_HEAP_LOCK();
  if(id>=sp_sym_str_cap){
    sp_int nc=sp_sym_str_cap?sp_sym_str_cap:64;
    while(nc<=id)nc*=2;
    const char**nt=(const char**)realloc((void*)sp_sym_str_tab,(size_t)nc*sizeof(const char*));
    if(nt){
      memset((void*)(nt+sp_sym_str_cap),0,(size_t)(nc-sp_sym_str_cap)*sizeof(const char*));
      sp_sym_str_tab=nt;sp_sym_str_cap=nc;
    }
  }
  const char*r=o->d;
  if(id<sp_sym_str_cap){
    if(sp_sym_str_tab[id]){free(o);r=sp_sym_str_tab[id];}   /* another thread won */
    else sp_sym_str_tab[id]=r;
  }
  SP_HEAP_UNLOCK();
  return r;
}

const char *sp_str_keep_copy(const char *s) { return sp_str_concat(s, sp_str_empty); }
/* A Hash's String key made from `len` bytes at `data`: a frozen handle over
   a copy, whose payload is the key (sp_hash_key_str, sp_hash_key_from_bytes) */
const char *sp_hash_key_from_bytes(const char *data, size_t len, int bin) {
  sp_String *h = sp_String_new_inline_len(data, (int64_t)len);
  if (bin) { h->binary = 1; sp_fd_publish(h); }
  sp_String_freeze(h);
  return h->data;
}
