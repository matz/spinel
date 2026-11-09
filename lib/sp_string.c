/* sp_string.c -- the cold sp_String in-place mutators (see sp_string.h).
   prepend / insert / replace / dup are off the hot string-building path, so they
   are compiled once here instead of inline in every generated TU. */
#include "sp_string.h"
#include "sp_str.h"   /* sp_str_concat (sp_str_keep_copy) */
#include <string.h>

/* Only evaluated literal handles join these permanent roots; unused
   generated slots cost no allocation or marking work. */
static sp_StringLiteral *sp_string_literals;
#ifdef SP_THREADS
static pthread_mutex_t sp_string_literal_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
sp_String *sp_String_literal_handle(sp_StringLiteral *slot, const char *s) {
  sp_String *h = SP_ATOMIC_LOAD(&slot->value, __ATOMIC_ACQUIRE);
  if (h) return h;
#ifdef SP_THREADS
  pthread_mutex_lock(&sp_string_literal_lock);
#endif
  h = slot->value;
  if (!h) {
    /* The source is static, and this bounded allocation cannot collect
       while the new handle is being linked into its permanent root. */
    h = sp_gc_alloc_nogc(sizeof *h, NULL, NULL);
    h->data = (char *)s;
    h->len = (int64_t)sp_str_byte_len(s);
    h->cap = h->len + 1;
    h->binary = sp_str_is_binary(s);
    sp_String_freeze(h);
    slot->next = sp_string_literals;
    sp_string_literals = slot;
    SP_ATOMIC_STORE(&slot->value, h, __ATOMIC_RELEASE);
  }
#ifdef SP_THREADS
  pthread_mutex_unlock(&sp_string_literal_lock);
#endif
  return h;
}
void sp_String_mark_literals(void) {
  for (sp_StringLiteral *p = sp_string_literals; p; p = p->next) sp_gc_mark(p->value);
}

void sp_String_prepend(sp_String*s,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("prepend"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(!sp_fd_grow(s,s->len+tl))return;memmove(s->data+tl,s->data,s->len+1);memcpy(s->data,t,tl);s->len+=tl;sp_fd_publish(s);}
/* String#insert(idx, str): insert at idx; negative idx is relative to len+1. */
void sp_String_insert(sp_String*s,int64_t idx,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("insert"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(tl==0)return;if(idx<0)idx+=s->len+1;if(idx<0)idx=0;if(idx>s->len)idx=s->len;if(!sp_fd_grow(s,s->len+tl))return;memmove(s->data+idx+tl,s->data+idx,s->len-idx+1);memcpy(s->data+idx,t,tl);s->len+=tl;sp_fd_publish(s);}
/* String#replace(s): replace entire content. */
void sp_String_replace(sp_String*s,const char*t){SP_GC_ROOT(s);SP_GC_ROOT_STR(t);if (SP_UNLIKELY(!s)) sp_nil_recv("replace"); if (!t)return;if(sp_String_is_frozen(s)){sp_raise_frozen_str(s->data);return;}int64_t tl=(int64_t)strlen(t);if(!sp_fd_grow(s,tl))return;memcpy(s->data,t,tl);s->data[tl]='\0';s->len=tl;sp_fd_publish(s);}
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

/* Symbol#to_s and #id2name: the symbol's name, frozen. CRuby 4.0 answers a
   chilled String that warns at its first mutation and is to be frozen; the
   program's string literals are frozen here already, and so is this one
   (docs/limitations.md). The symbol table's own name is a plain static
   without a header, so each symbol gets one frozen copy with a header (0xf1,
   its link sp_sym_str_tag and no literal slot: a handle made of it is a new
   frozen handle), built at its
   first to_s and kept as long as the symbol is; the collector neither marks
   nor sweeps it, and its header keeps the length and the hash. The table is
   guarded by the heap lock, as the dedup table is. */
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

struct sp_String_s *sp_strelem_of(const char *b) { return sp_String_new_shared(b); }
const char *sp_str_keep_copy(const char *s) { return sp_str_concat(s, sp_str_empty); }
/* A Hash's String key made from `len` bytes at `data`: a frozen handle over
   a copy, whose payload is the key (sp_hash_key_str, sp_hash_key_from_bytes) */
const char *sp_hash_key_from_bytes(const char *data, size_t len, int bin) {
  sp_String *h = sp_String_new_inline_len(data, (int64_t)len);
  if (bin) { h->binary = 1; sp_fd_publish(h); }
  sp_String_freeze(h);
  return h->data;
}
