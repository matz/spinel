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
   mutation rather than living only in the bytes. `chilled` is nonzero for a
   handle made from a chilled String (sp_str_is_chilled: what Symbol#to_s
   answers): that symbol's id + 1, so sp_String_uplus can tell whether the
   bytes are still its name -- +@ copies it then, as it copies a frozen one,
   and a mutated one is an ordinary String, as CRuby's is. A GC allocation is
   zeroed, so every other handle starts at 0 with no store. */
typedef struct { char *data; int64_t len; int64_t cap; unsigned binary; unsigned chilled; } sp_String;
/* sp_fd_publish reads `binary` and `chilled` as one word */
_Static_assert(offsetof(sp_String, chilled) == offsetof(sp_String, binary) + 4 && sizeof(unsigned) == 4,
               "sp_String.binary and .chilled are adjacent 32-bit fields");

/* A generated frozen literal owns its handle slot. The tagged header link
   distinguishes that slot from the aligned links in the string heap. */
typedef struct sp_StringLiteral {
  sp_String *value;
  struct sp_StringLiteral *next;
} sp_StringLiteral;
static inline sp_StringLiteral *sp_String_literal_slot(const char *s) {
  if (!s || ((const unsigned char *)s)[-1] != 0xf1) return NULL;
  uintptr_t p = (uintptr_t)(((const sp_str_hdr *)(s - 1)) - 1)->next;
  return p & 1 ? (sp_StringLiteral *)(p - 1) : NULL;
}
sp_String *sp_String_literal_handle(sp_StringLiteral *slot, const char *s);
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
  /* Both flags are rare and read as one word, so a plain handle pays the one
     load and branch the binary tag alone did. A mutation makes a chilled
     handle plain for good, as CRuby's str_modify does, even one that leaves
     the bytes the symbol's name again (`m << "!"; m.chop!`). */
  uint64_t fl; memcpy(&fl, &s->binary, sizeof fl);
  if (SP_EXPECT(fl != 0, 0)) {
    if (s->binary) h->size |= SP_STR_SIZE_BINARY;
    s->chilled = 0;
  }
  sp_str_lcache_drop(s->data);
}
/* A handle whose payload sits inside its own GC object, right after the
   struct (sp_String_new_fresh): no malloc and no finalizer, which are most of
   what a handle costs to make and to collect. Its first growth moves the
   payload out (sp_fd_grow_inline, lib/sp_string.c), off the inlined path. */
static inline int sp_fd_is_inline(sp_String *s){return sp_fd_base(s->data)==(char*)(s+1);}
SP_COLD SP_NOINLINE int sp_fd_grow_inline(sp_String *s, int64_t need);
static inline int sp_fd_grow(sp_String *s, int64_t need){
  if (need < s->cap) return 1;
  if (SP_EXPECT(sp_fd_is_inline(s), 0)) return sp_fd_grow_inline(s, need);
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)s - sizeof(sp_gc_hdr));
  int64_t new_cap = (need * 2) + 16;
  sp_str_lcache_drop(s->data);
  char *raw = (char *)realloc(sp_fd_base(s->data), SP_FD_OVH + new_cap);
  if (!raw) return 0;
  sp_gc_bytes_sub(s->cap + SP_FD_OVH); h->size -= s->cap + SP_FD_OVH;
  s->cap = new_cap; s->data = sp_fd_setup(raw); sp_fd_own(s);
  h->size += s->cap + SP_FD_OVH; sp_gc_bytes_add(s->cap + SP_FD_OVH);
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
  r->data=sp_fd_setup(raw);r->data[0]=0;r->len=0;r->cap=cap;r->binary=0;r->chilled=0;sp_fd_own(r);
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
static inline void sp_fd_append_len(sp_String*s,const char*t,int64_t tl){if(!sp_fd_grow(s,s->len+tl))return;memcpy(s->data+s->len,t,tl);s->len+=tl;s->data[s->len]=0;sp_fd_publish(s);}
static inline void sp_String_append(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_fd_append_len(s,t,(int64_t)strlen(t));}
/* Binary-safe append: sizes the operand with the header length so an embedded
   NUL is preserved (Ruby String#<< / concat on a marked spinel string). */
/* Replace the buffer contents in place (the handle stays stable, so every
   alias and container holding it observes the new value; #3227). */
static inline void sp_String_set_bin(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}s->len=0;sp_fd_append_len(s,t,(int64_t)sp_str_byte_len(t));}
/* The bytes a read put into its (len, outbuf) buffer: binary-safe, and the
   buffer becomes ASCII-8BIT, as CRuby's readpartial/sysread/read_nonblock
   leave it. */
static inline void sp_String_set_read_bytes(sp_String*s,const char*t){if(!s||!t)return;sp_String_set_bin(s,t);s->binary=1;sp_fd_publish(s);}
/* the first tl bytes of t: the append form of an interpolation (emit_interp_append) */
static inline void sp_String_append_n(sp_String*s,const char*t,size_t tl){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_fd_append_len(s,t,(int64_t)tl);}
/* append_as_bytes preserves the handle's encoding as well as embedded NULs. */
static inline void sp_String_append_bytes(sp_String*s,const char*t){if(!s||!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}sp_fd_append_len(s,t,(int64_t)sp_str_byte_len(t));}
static inline void sp_String_append_bin(sp_String*s,const char*t){
  if(!s||!t)return;
  if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}
  if (s->binary && !sp_str_is_binary(t) &&
      !sp_str_ascii_only(t) && sp_str_ascii_only(s->data)) {
    s->binary=0;
    sp_str_as_text(s->data);
  }
  sp_fd_append_len(s,t,(int64_t)sp_str_byte_len(t));
}
/* Handle wrap for CODEGEN-emitted sources only: every spinel-emitted string
   carries a marker byte at s[-1], so the frozen state (0xf1: an explicit
   .freeze / frozen_string_literal) can be inherited safely. Runtime-internal
   callers pass bare C literals with NO marker -- they must use the plain
   sp_String_new above (reading s[-1] there is OOB and, under clang's rodata
   layout, misreads as frozen; cf. the #282 marker-probe lesson). */
/* Chilled Strings (sp_str_is_chilled), compiled once in lib/sp_string.c and
   kept off the inlined constructors and +@: sp_String_chill names the symbol
   a new handle came from (a 0xfb static is rare, a chilled one rarer),
   sp_String_chilled_now asks whether a handle is still that symbol's name,
   and sp_sym_to_s_chilled is Symbol#to_s. */
SP_COLD void sp_String_chill(sp_String*r,const char*s);
int sp_String_chilled_now(sp_String*h);
const char*sp_sym_to_s_chilled(sp_sym id);
static inline sp_String*sp_String_new_shared(const char*s){
  if(!s)return NULL;   /* nil into a shared-string slot stays nil (a nullable String, #4567) */
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
#ifdef SP_SHARE_STRING_LITERALS
  sp_StringLiteral *literal = sp_String_literal_slot(s);
  if (literal) return sp_String_literal_handle(literal, s);
#endif
  int64_t len=(int64_t)sp_str_byte_len(s);
  sp_String*r=sp_String_new_len(s,len);
  if(bin){r->binary=1;sp_fd_publish(r);}
  if(frozen){sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->frozen=1;}
  if(SP_UNLIKELY(mk==0xfb))sp_String_chill(r,s);   /* a static: still there after the allocation */
  return r;
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
  int bin=sp_str_is_binary(s);
  int frozen=sp_str_is_frozen_val(s);
  int mk=((const unsigned char*)s)[-1];
#ifdef SP_SHARE_STRING_LITERALS
  sp_StringLiteral *literal = sp_String_literal_slot(s);
  if (literal) return sp_String_literal_handle(literal, s);
#endif
  int64_t len=(int64_t)sp_str_byte_len(s);
  sp_String*r=sp_String_new_inline_len(s,len);
  if(bin){r->binary=1;sp_fd_publish(r);}
  if(frozen){sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->frozen=1;}
  if(SP_UNLIKELY(mk==0xfb))sp_String_chill(r,s);   /* a static: still there after the allocation */
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
static inline int64_t sp_String_length(sp_String*s){return s->len;}

/* --share-strings: a handle's own bytes (0xfd) as a box of that handle,
   its header's `next` (sp_fd_own), while the handle still has these bytes;
   otherwise the bytes boxed as themselves. */
static inline sp_RbVal sp_box_str_owner(const char *v) {
  sp_String *h = (sp_String *)(void *)(((const sp_str_hdr *)(v - 1)) - 1)->next;
  if (h && h->data == v) return sp_box_obj(h, SP_BUILTIN_STRBUF);
  return sp_box_str_bytes(v);
}
/* A String no other name holds, boxed (sp_box_str's rule otherwise):
   --share-strings boxes a mutable heap String as a new handle over its
   bytes, which sp_String_new_fresh copies before it allocates, so every
   copy of the box names the one String; a frozen or static String is boxed
   as itself, as nothing can change it and its identity is its bytes'. */
static inline sp_RbVal sp_box_str_new(const char *v) {
#ifdef SP_SHARE_BOX_HANDLES
  if (v) {
    unsigned char m = ((const unsigned char *)v)[-1];
    if (m == 0xfe || m == 0xfc) return sp_box_obj(sp_String_new_fresh(v), SP_BUILTIN_STRBUF);
  }
#endif
  return sp_box_str(v);
}

/* Cold in-place mutators (compiled once in lib/sp_string.c). */
void sp_String_prepend(sp_String*s,const char*t);
void sp_String_insert(sp_String*s,int64_t idx,const char*t);
void sp_String_replace(sp_String*s,const char*t);
sp_String*sp_String_dup(sp_String*s);
#endif /* SP_STRING_H */
