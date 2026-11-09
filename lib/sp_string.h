#ifndef SP_STRING_H
#define SP_STRING_H
/* sp_string.h -- the mutable-String builder (sp_String), shared so both the
   generated TU and cold lib C files (inspect helpers, ...) can build strings.

   The hot core -- construction, append, and the sp_fd_* buffer mechanics that
   sit on the string concat / interpolation path -- stays `static inline` here so
   every TU inlines it (no perf cost vs. living in spinel_rt.h). The rarely-used
   in-place mutators (prepend / insert / replace / dup) are cold, so they are
   compiled once into libspinel_rt.a (lib/sp_string.c). */
#include "sp_alloc.h"   /* sp_gc_alloc, sp_gc_bytes/hdr, sp_str_hdr, sp_str_byte_len, sp_raise_cls */
#include <string.h>

int sp_str_ascii_only(const char *s);

/* `binary` is the ASCII-8BIT tag, kept on the HANDLE: sp_fd_setup zeroes the
   payload header on every grow, so the tag has to be re-stamped after each
   mutation rather than living only in the bytes. A GC allocation is
   zeroed, so every other handle starts at 0 with no store. */
typedef struct sp_String_s sp_String;   /* the struct is in sp_types.h, beside sp_strelem */

/* A generated frozen literal owns its handle slot. The tagged header link
   distinguishes that slot from the aligned links in the string heap. */
typedef struct sp_StringLiteral {
  sp_String *value;
  struct sp_StringLiteral *next;
} sp_StringLiteral;
static inline sp_StringLiteral *sp_String_literal_slot(const char *s) {
  if (!s || ((const unsigned char *)s)[-1] != 0xf1) return NULL;
  const sp_str_hdr *h = ((const sp_str_hdr *)(s - 1)) - 1;
  /* a slab string's link is never written (its chunk's bitmaps hold what
     the link would): a frozen one keeps its slot's last occupant's first
     word there, which may be odd */
  if (sp_slab_owns(h)) return NULL;
  uintptr_t p = (uintptr_t)h->next;
  return p & 1 ? (sp_StringLiteral *)(p - 1) : NULL;
}
sp_String *sp_String_literal_handle(sp_StringLiteral *slot, const char *s);
/* The handle of a literal the generated code names with its slot (_fzl_N):
   the slot read directly, where sp_String_new_shared asks the marks, the
   header and the slab first, about 20 instructions a store (`@tag = "n"`
   in each bm_gcbench Node) */
static inline sp_String *sp_String_literal_of(sp_StringLiteral *slot, const char *s) {
  sp_String *h = SP_ATOMIC_LOAD(&slot->value, __ATOMIC_ACQUIRE);
  return h ? h : sp_String_literal_handle(slot, s);
}
void sp_String_mark_literals(void);
static inline const void *sp_String_identity(sp_String *h) {
  return h && sp_String_literal_slot(h->data) ? (const void *)h->data : (const void *)h;
}

/* Per-mutable-string freeze flag rides in the GC header alongside `marked`. */
static inline sp_bool sp_String_is_frozen(sp_String*s){if(!s)return TRUE;sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));return h->frozen;}
static inline sp_String*sp_String_freeze(sp_String*s){if(s){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));h->frozen=1;}return s;}

/* A mutable String's payload carries the same length-bearing sp_str_hdr that
   0xfe/0xfc heap strings use, so an escaped const char* is binary-safe (the
   reader uses sp_str_byte_len, not strlen). Block layout:
   [sp_str_hdr][0xfd marker][data ...][NUL]. s->data points at `data`. The ctor
   and in-place mutators are also reached with bare C string literals (no marker
   byte), so they size their operand with strlen, not a [-1]-marker reader. */
#define SP_FD_HDR (sizeof(sp_str_hdr)+1)   /* header + marker byte */
#define SP_FD_OVH (sizeof(sp_str_hdr)+2)   /* header + marker + NUL terminator */
static inline char *sp_fd_base(const char *data){return (char*)data-SP_FD_HDR;}
/* The payload's `next` field carries the owning sp_String rather than a heap
   link: these blocks are malloc'd and never join the string heap, so nothing
   walks that list, and a container holding the escaped `const char *` has no
   other way back to the handle. Without it the mark reaches the bytes and
   stops, the handle goes unreferenced, and sp_String_fin frees the bytes the
   container still points at. sp_fd_own publishes it; every path that lays out
   a payload has to call it. */
static inline char *sp_fd_setup(char *raw){
  sp_str_hdr *h = (sp_str_hdr *)raw;
  h->next = NULL; h->size = 0; h->len = 0; h->hash = 0;
  char *body = (char *)(h + 1);
  body[0] = (char)0xfd;
  return body + 1;
}
static inline void sp_fd_own(sp_String *s){
  ((sp_str_hdr *)sp_fd_base(s->data))->next = (sp_str_hdr *)(void *)s;
}
static inline void sp_fd_publish(sp_String *s){
  sp_str_hdr *h = (sp_str_hdr *)sp_fd_base(s->data);
  h->len = (uint32_t)s->len; h->hash = 0;
  h->size &= ~SP_STR_SIZE_ASCII7;   /* the bytes just changed */
  if (SP_EXPECT(s->binary != 0, 0)) h->size |= SP_STR_SIZE_BINARY;
  sp_str_lcache_drop(s->data);
}
/* A handle whose payload sits inside its own GC object, right after the
   struct (sp_String_new_fresh): no malloc and no finalizer, which are most of
   what a handle costs to make and to collect. Its first growth moves the
   payload out (sp_fd_grow_inline, lib/sp_string.c), off the inlined path. */
static inline int sp_fd_is_inline(sp_String *s){return sp_fd_base(s->data)==(char*)(s+1);}
SP_COLD SP_NOINLINE int sp_fd_grow_inline(sp_String *s, int64_t need);
/* An outside payload a growth moved is not freed there: a view of its bytes
   may still be held (a root, a Hash key, a box), and the mark reads the
   owner link through it. sp_fd_retire marks it 0xf9 and keeps it until a
   full cycle's mark reaches no view of it (lib/sp_gc.c). */
void sp_fd_retire(char *base);
static inline int sp_fd_grow(sp_String *s, int64_t need){
  if (need < s->cap) return 1;
  if (SP_EXPECT(sp_fd_is_inline(s), 0)) return sp_fd_grow_inline(s, need);
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)s - sizeof(sp_gc_hdr));
  int64_t new_cap = (need * 2) + 16;
  sp_str_lcache_drop(s->data);
  char *old = sp_fd_base(s->data);
  char *raw = (char *)malloc(SP_FD_OVH + new_cap);
  if (!raw) return 0;
  char *data = sp_fd_setup(raw);
  /* the whole old room, as sp_fd_grow_inline copies it */
  memcpy(data, s->data, (size_t)s->cap + 1);
  h->size -= s->cap + SP_FD_OVH;
  s->cap = new_cap; s->data = data; sp_fd_own(s);
  h->size += s->cap + SP_FD_OVH; sp_gc_bytes_add(s->cap + SP_FD_OVH);
  sp_fd_retire(old);
  return 1;
}
static inline void sp_String_fin(void*p){sp_str_lcache_drop(((sp_String*)p)->data);free(sp_fd_base(((sp_String*)p)->data));}
/* Build a handle over an explicit byte length. Everything the caller needs to
   read out of `s` must be read BEFORE this returns: the copy below happens
   ahead of sp_gc_alloc precisely because a collection there frees an `s` that
   this stack frame is the only anchor for (roots are explicit -- the C stack is
   not scanned), so `s` may be dangling by the time it comes back. */
static inline sp_String*sp_String_new_len(const char*s,int64_t len){
  int64_t cap=(len*2)+16;
  char*raw=(char*)malloc(SP_FD_OVH+cap);
  char*data=sp_fd_setup(raw);
  memcpy(data,s,len);data[len]=0;
  sp_String*r=(sp_String*)sp_gc_alloc(sizeof(sp_String),sp_String_fin,NULL);
  r->len=len;r->cap=cap;r->data=data;r->binary=0;sp_fd_own(r);
  {sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->size+=r->cap+SP_FD_OVH;sp_gc_bytes_add(r->cap+SP_FD_OVH);}
  sp_fd_publish(r);
  return r;
}
static inline sp_String*sp_String_new(const char*s){return sp_String_new_len(s,(int64_t)strlen(s));}
/* The empty String a String subclass instance starts with, inside the
   instance (a GC object whose finalizer is sp_String_fin): the handle's
   payload is malloc'd and linked back to the handle, which is the instance. */
static inline void sp_String_init_embedded(sp_String*r){
  int64_t cap=16;
  char*raw=(char*)malloc(SP_FD_OVH+cap);
  if(!raw)sp_oom_die();
  r->data=sp_fd_setup(raw);r->data[0]=0;r->len=0;r->cap=cap;r->binary=0;sp_fd_own(r);
  {sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->size+=r->cap+SP_FD_OVH;sp_gc_bytes_add(r->cap+SP_FD_OVH);}
  sp_fd_publish(r);
}
/* The same over a payload inside the object, when it fits: for a String the
   handle is made for and nobody else holds, a literal or a temporary handed to
   a parameter that is the handle (#6179), made and dropped once per call. The
   bytes are read into a stack copy first, for the reason sp_String_new_len
   copies before it allocates. */
#define SP_FD_INLINE_MAX 192
static inline sp_String*sp_String_new_inline_len(const char*s,int64_t len){
  int64_t cap=(len*2)+16;
  size_t sz=sizeof(sp_String)+SP_FD_OVH+(size_t)cap;
  if(sz>SP_FD_INLINE_MAX)return sp_String_new_len(s,len);
  char tmp[SP_FD_INLINE_MAX];
  memcpy(tmp,s,(size_t)len);
  sp_String*r=(sp_String*)sp_gc_alloc(sz,NULL,NULL);
  char*data=sp_fd_setup((char*)(r+1));
  memcpy(data,tmp,(size_t)len);data[len]=0;
  r->len=len;r->cap=cap;r->data=data;r->binary=0;sp_fd_own(r);
  sp_fd_publish(r);
  return r;
}
/* Shared append core: `tl` is the operand byte length (strlen for the
   bare-literal-safe entry, sp_str_byte_len for the binary one). */
/* the operand may be a view of this very payload (`s << s`, `s.replace(s)`):
   its offset survives the move a growth makes, its pointer does not */
static inline void sp_fd_append_len(sp_String*s,const char*t,int64_t tl){
  int64_t off=(uintptr_t)t>=(uintptr_t)s->data&&(uintptr_t)t<=(uintptr_t)(s->data+s->len)?(int64_t)(t-s->data):-1;
  if(!sp_fd_grow(s,s->len+tl))return;
  if(off>=0)t=s->data+off;
  memmove(s->data+s->len,t,(size_t)tl);s->len+=tl;s->data[s->len]=0;sp_fd_publish(s);
}
static inline void sp_String_append(sp_String*s,const char*t){if (SP_UNLIKELY(!s)) sp_nil_recv("<<"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_fd_append_len(s,t,(int64_t)strlen(t));}
/* Binary-safe append: sizes the operand with the header length so an embedded
   NUL is preserved (Ruby String#<< / concat on a marked spinel string). */
/* Replace the buffer contents in place (the handle stays stable, so every
   alias and container holding it observes the new value; #3227). */
static inline void sp_String_set_bin(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}s->len=0;sp_fd_append_len(s,t,(int64_t)sp_str_byte_len(t));}
/* The bytes a read put into its (len, outbuf) buffer: binary-safe, and the
   buffer becomes ASCII-8BIT, as CRuby's readpartial/sysread/read_nonblock
   leave it. */
static inline void sp_String_set_read_bytes(sp_String*s,const char*t){if(!s||!t)return;sp_String_set_bin(s,t);s->binary=1;sp_fd_publish(s);}
/* sp_String_set_bin taking t's encoding as well: a splice whose result
   the compatibility rule made binary (or text) leaves the handle so */
static inline void sp_String_set_bin_enc(sp_String*s,const char*t){
  if(!s||!t)return;
  sp_String_set_bin(s,t);
  if(sp_str_is_binary(t)){s->binary=1;sp_fd_publish(s);}
  else if(s->binary){s->binary=0;sp_str_as_text(s->data);sp_fd_publish(s);}
}
/* The encoding `s << t` leaves s in, read before t's tl bytes go in, as
   CRuby's compatibility rule picks it (sp_str_cat_binary): an ASCII-only
   operand keeps the receiver's, an ASCII-only receiver takes the operand's
   -- `+"hdr" << png.b` is ASCII-8BIT -- and two runs with high bytes in
   different encodings stay binary. The bytes are scanned only when the two
   encodings differ. */
static inline void sp_String_append_enc(sp_String*s,const char*t,size_t tl){
  int tb=sp_str_is_binary(t);
  if(SP_EXPECT((int)s->binary==tb,1))return;
  for(size_t i=0;i<tl;i++)
    if((unsigned char)t[i]>=0x80){
      if(sp_str_ascii_only(s->data)&&!tb){s->binary=0;sp_str_as_text(s->data);}
      else s->binary=1;
      return;
    }
}
/* the first tl bytes of t: the append form of an interpolation (emit_interp_append) */
static inline void sp_String_append_n(sp_String*s,const char*t,size_t tl){if (SP_UNLIKELY(!s)) sp_nil_recv("<<"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_String_append_enc(s,t,tl);sp_fd_append_len(s,t,(int64_t)tl);}
/* append_as_bytes preserves the handle's encoding as well as embedded NULs. */
static inline void sp_String_append_bytes(sp_String*s,const char*t){if (SP_UNLIKELY(!s)) sp_nil_recv("<<"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_fd_append_len(s,t,(int64_t)sp_str_byte_len(t));}
static inline void sp_String_append_bin(sp_String*s,const char*t){
  if (SP_UNLIKELY(!s)) sp_nil_recv("<<"); if (!t)return;
  if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}
  size_t tl=sp_str_byte_len(t);
  sp_String_append_enc(s,t,tl);
  sp_fd_append_len(s,t,(int64_t)tl);
}
/* Handle wrap for CODEGEN-emitted sources only: every spinel-emitted string
   carries a marker byte at s[-1], so the frozen state (0xf1: an explicit
   .freeze / frozen_string_literal) can be inherited safely. Runtime-internal
   callers pass bare C literals with NO marker -- they must use the plain
   sp_String_new above (reading s[-1] there is OOB and, under clang's rodata
   layout, misreads as frozen; cf. the #282 marker-probe lesson). */
/* Symbol#to_s and #id2name: the symbol's name, frozen (lib/sp_string.c) */
const char*sp_sym_to_s_frozen(sp_sym id);
/* With literal slots, the handle a literal or a live payload already has is
   found inline, and only a String that needs a new handle calls the
   constructor below: its frame (six saved registers and the stack guard)
   cost a literal's store about 45 instructions (`@e = "c"` in a by-value
   object's initialize, bm_object_new_init). */
/* U2 (string-u2-design.md): the handle a heap String was born inside
   (sp_str_alloc_born), or NULL for one that was not: the header's link names
   the handle right in front of it */
static inline sp_String*sp_str_born_owner(const char*s){
  const sp_str_hdr*h=((const sp_str_hdr*)(s-1))-1;
  sp_String*o=(sp_String*)(void*)h->next;
  return o&&(const char*)(o+1)==(const char*)h?o:NULL;
}
/* A slot takes such a String: its handle becomes the String, over the bytes
   where they are (the length and the binary tag from the header, a frozen
   mark into the handle's), and the bytes become its payload (0xfd). No
   allocation and no copy: what wrapping a builtin's answer cost. */
static inline sp_String*sp_String_adopt(const char*s,sp_String*o){
  sp_str_hdr*h=((sp_str_hdr*)(s-1))-1;
  unsigned char mk=((const unsigned char*)s)[-1];
  o->len=(int64_t)h->len;
  o->binary=(h->size&SP_STR_SIZE_BINARY)!=0;
  if(mk!=0xfe)sp_String_freeze(o);
  ((unsigned char*)s)[-1]=0xfd;
  return o;
}
/* A static of one byte or none (0xff: the character table s[i] answers
   from, and the empty String) has no header, mark or literal slot to ask:
   its handle is laid out directly, as sp_String_new_inline_len would lay it
   out. Taking a character into a local made one through the general
   constructor, about 200 instructions of the 300 bm_json_parse spent per
   character over the default build. */
static SP_NOINLINE sp_String*sp_String_new_static1(unsigned char c){
  enum{CAP=1*2+16};
  sp_String*r=(sp_String*)sp_gc_alloc_sized(sizeof(sp_String)+SP_FD_OVH+CAP,NULL);
  /* the object comes zeroed: the header's hash, the binary word and
     the NUL need no store */
  sp_str_hdr*h=(sp_str_hdr*)(void*)(r+1);
  char*data=(char*)(h+1)+1;
  h->next=(sp_str_hdr*)(void*)r;h->len=c!=0;
  data[-1]=(char)0xfd;data[0]=(char)c;
  r->data=data;r->len=c!=0;r->cap=CAP;
  sp_str_lcache_drop(data);
  /* (one byte below 0x80, or none, is 7-bit: indexing it is byte indexing) */
  if(c<0x80)h->size=SP_STR_SIZE_ASCII7;
  return r;
}
static SP_NOINLINE sp_String*sp_String_new_shared_make(const char*s);
static inline sp_String*sp_String_new_shared(const char*s){
  if(!s)return NULL;
  unsigned char mk=((const unsigned char*)s)[-1];
  if(mk==0xfd){sp_String*o=(sp_String*)(void*)((sp_str_hdr*)sp_fd_base(s))->next;if(o)return o;}
  if(mk==0xfe||mk==0xfa){sp_String*o=sp_str_born_owner(s);if(o)return sp_String_adopt(s,o);}
  if(mk==0xff&&(!s[0]||!s[1]))return sp_String_new_static1((unsigned char)s[0]);
  if(mk==0xf1){sp_StringLiteral*l=sp_String_literal_slot(s);if(l){sp_String*lh=SP_ATOMIC_LOAD(&l->value,__ATOMIC_ACQUIRE);if(lh)return lh;}
    /* (a heap String .freeze made 0xf1 is no literal: its handle, when it
       was born in one) */
    else{sp_String*o=sp_str_born_owner(s);if(o)return sp_String_adopt(s,o);}
  }
  return sp_String_new_shared_make(s);
}
static SP_NOINLINE sp_String*sp_String_new_shared_make(const char*s){
  if(!s)return NULL;   /* nil into a shared-string slot stays nil (a nullable String, #4567) */
  /* one representation: the bytes of a live payload are the handle its
     owner link names (sp_fd_own), which keeps the String's identity */
  if(((const unsigned char*)s)[-1]==0xfd){sp_String*o=(sp_String*)(void*)((sp_str_hdr*)sp_fd_base(s))->next;if(o)return o;}
  /* Read every property of `s` HERE, before the allocation below: the handle's
     constructor can collect, and `s` is typically an unrooted temporary (the
     codegen hands this `sp_IntArray_pack(...)` directly), so touching it
     afterwards is a read of freed memory.

     The ASCII-8BIT tag is inherited, not just the frozen bit: a pack / String#b
     result captured by a block becomes one of these handles, and dropping the
     tag put its bytes back on the character-counting path. The LENGTH has to
     come with it. strlen stops at the first NUL, so sizing a binary payload
     that way truncated it: `Array.new(n, 0).pack("C*")` became the empty string
     the moment it was stored anywhere that promotes it, and the next setbyte
     raised "index 0 out of string" against a zero-length buffer.

     The length comes from the header for EVERY marked string, not only a
     binary-tagged one. A string of NUL bytes is all-ASCII, so the binary bit
     is off and strlen answered 0 for it too: `Box.new("\0" * 8)` whose ivar is
     later mutated through its reader -- the promotion that brings a handle
     here -- arrived empty, and the setbyte raised against the zero-length
     buffer. sp_str_byte_len reads the header length and falls back to strlen
     only for an UNMARKED string, which this constructor is never given. */
  int bin=sp_str_is_binary(s);
  int frozen=sp_str_is_frozen_val(s);
  int mk=((const unsigned char*)s)[-1];
  /* (one representation: the runtime takes a literal's bytes into an
     element itself, sp_strelem_of, so its own copy asks the slot too) */
  sp_StringLiteral *literal = sp_String_literal_slot(s);
  if (literal) { sp_String *lh = SP_ATOMIC_LOAD(&literal->value, __ATOMIC_ACQUIRE); return lh ? lh : sp_String_literal_handle(literal, s); }
  int64_t len=(int64_t)sp_str_byte_len(s);
  /* one representation: every String a slot takes becomes a handle, so the
     payload is laid out inside the object when it fits (no malloc, no
     finalizer); its first growth moves it out (sp_fd_grow_inline) */
  sp_String*r=sp_String_new_inline_len(s,len);
  if(bin){r->binary=1;sp_fd_publish(r);}
  if(frozen){sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->frozen=1;}
  return r;
}
/* The handle a Hash key stands for (sp_String_key_handle): a frozen handle's payload, which
   sp_hash_key_str keeps as the key, answers that handle through the owner
   link; any other key (a frozen copy, a static) a new frozen handle over it. */
static inline sp_String*sp_String_key_handle(const char*k){
  if(!k)return NULL;
  if(((const unsigned char*)k)[-1]==0xfd){sp_String*o=(sp_String*)(void*)((sp_str_hdr*)sp_fd_base(k))->next;if(o)return o;}
  return sp_String_new_shared(k);
}
/* An expression of either String face as the shared handle: a handle is
   itself, a plain String is wrapped in a fresh one. The emitter writes it where
   a typed handle parameter is filled from an expression whose own emitter
   answers either (`+""`, a fresh String's builtin), and does not know which. */
static inline sp_String*sp_string_handle_id(sp_String*h){return h;}
#define SP_AS_STRING_HANDLE(x) _Generic((x), sp_String *: sp_string_handle_id, default: sp_String_new_shared)(x)
/* sp_String_new_shared for a String no one else holds (a literal's copy, a
   temporary, a plain String a handle parameter reads off the boxed channel):
   the same length and marks, over a payload inside the object. */
static inline sp_String*sp_String_new_fresh(const char*s){
  if(!s)return NULL;
  if(((const unsigned char*)s)[-1]==0xfd){sp_String*o=(sp_String*)(void*)((sp_str_hdr*)sp_fd_base(s))->next;if(o)return o;}
  /* one representation: a heap byte string a builtin answered is a temporary
     no other name holds; the handle adopts its bytes (sp_String_new_shared) */
  if(((const unsigned char*)s)[-1]==0xfe)return sp_String_new_shared(s);
  int bin=sp_str_is_binary(s);
  int frozen=sp_str_is_frozen_val(s);
  int mk=((const unsigned char*)s)[-1];
  /* (one representation: the runtime takes a literal's bytes into an
     element itself, sp_strelem_of, so its own copy asks the slot too) */
  sp_StringLiteral *literal = sp_String_literal_slot(s);
  if (literal) { sp_String *lh = SP_ATOMIC_LOAD(&literal->value, __ATOMIC_ACQUIRE); return lh ? lh : sp_String_literal_handle(literal, s); }
  int64_t len=(int64_t)sp_str_byte_len(s);
  sp_String*r=sp_String_new_inline_len(s,len);
  if(bin){r->binary=1;sp_fd_publish(r);}
  if(frozen){sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->frozen=1;}
  return r;
}
/* ...and for `+"lit"`: a mutable copy of a literal, which is frozen itself */
static inline sp_String*sp_String_new_unfrozen(const char*s){
  if(!s)return NULL;
  int bin=sp_str_is_binary(s);
  sp_String*r=sp_String_new_inline_len(s,(int64_t)sp_str_byte_len(s));
  if(bin){r->binary=1;sp_fd_publish(r);}
  return r;
}
/* force_encoding / encode! on a handle: the ASCII-8BIT tag goes on the
   handle, where every later growth re-stamps it from (sp_fd_publish), and
   into the bytes now; mode 1 sets it, 0 clears it, -1 (another encoding)
   leaves it. A frozen String refuses either way, as CRuby does (#3334). */
static inline sp_String*sp_String_force_encoding(sp_String*s,int mode){
  if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return s;}
  if(mode<0)return s;
  s->binary=(unsigned)mode;
  sp_str_hdr*h=(sp_str_hdr*)sp_fd_base(s->data);
  if(mode)h->size|=SP_STR_SIZE_BINARY;else h->size&=~SP_STR_SIZE_BINARY;
  sp_str_lcache_drop(s->data);
  return s;
}
static inline const char*sp_String_cstr(sp_String*s){return s->data;}
/* the bytes of a handle that can be nil, NULL for nil, as its copying read
   (sp_strbuf_read) answers */
static inline const char*sp_String_cstr_nil(sp_String*s){return s?s->data:NULL;}
static inline int64_t sp_String_length(sp_String*s){return s->len;}

/* A String no other name holds, boxed for something that keeps the box:
   boxes a mutable heap String as a new handle over its bytes,
   which sp_String_new_fresh copies before it allocates, so every copy of
   the box names the one String. So is an unfrozen static one (0xff, a
   folded interpolation's bytes): it stands for a new String, which a
   mutation through the box changes (`a = ["#{v}?"]; a[0] << "!"`). A
   frozen String is boxed as itself, as nothing can change it and its
   identity is its bytes'; so is any String otherwise (sp_box_str). */
static inline sp_RbVal sp_box_str_new(const char *v) {
  if (v) {
    unsigned char m = ((const unsigned char *)v)[-1];
    if (m == 0xfe || m == 0xfc || m == 0xff) return sp_box_obj(sp_String_new_fresh(v), SP_BUILTIN_STRBUF);
    /* the runtime's empty answer is static, but stands for the new empty
       String the call made (`"" * 2`): nil.to_s's frozen one is not this */
    if (v == sp_str_empty) return sp_box_obj(sp_String_new_fresh(v), SP_BUILTIN_STRBUF);
  }
  return sp_box_str(v);
}

/* A new String a call answers in bytes it may share with others (Integer#chr
   answers a one-character table's), boxed for something that keeps the box:
   a new handle over a copy of them, which an append through any copy of the
   box changes. */
static inline sp_RbVal sp_box_str_fresh(const char *v) {
  if (v) return sp_box_obj(sp_String_new_fresh(v), SP_BUILTIN_STRBUF);
  return sp_box_str(v);
}

/* encode! on a handle, given the String encode answered for it (lib/sp_cold.c) */
void sp_String_encode_into(sp_String *h, const char *t);
/* Cold in-place mutators (compiled once in lib/sp_string.c). */
void sp_String_prepend(sp_String*s,const char*t);
void sp_String_insert(sp_String*s,int64_t idx,const char*t);
void sp_String_replace(sp_String*s,const char*t);
sp_String*sp_String_dup(sp_String*s);
#endif /* SP_STRING_H */
