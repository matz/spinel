/* Spinel Runtime Library */
#ifndef SP_RUNTIME_H
#define SP_RUNTIME_H

#ifdef __APPLE__
/* getcontext/makecontext/swapcontext are gated behind _XOPEN_SOURCE on
   Darwin and marked deprecated since 10.6. _DARWIN_C_SOURCE re-enables
   Darwin extensions (MAP_ANON, etc.) that _XOPEN_SOURCE alone would hide.
   Suppress the deprecation warning so -Werror builds pass — we knowingly
   use these APIs because Spinel's Fiber implementation depends on them. */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
/* POSIX compat shims for MinGW */
#define mmap(a,l,p,f,fd,off) VirtualAlloc(NULL,(l),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)
#define munmap(a,l) (VirtualFree((a),0,MEM_RELEASE)?0:-1)
#define MAP_FAILED NULL
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define MAP_NORESERVE 0
#else
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
#include <malloc.h>
#else
/* Darwin's libc has no malloc_trim; make it a no-op so call sites stay portable. */
#define malloc_trim(x) ((void)0)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

typedef int64_t mrb_int;
typedef double mrb_float;
typedef bool mrb_bool;
/* sp_sym is defined per-program in emit_sym_runtime, but poly helpers
   below need to reference it by forward declaration. */
typedef mrb_int sp_sym;
static const char *sp_sym_to_s(sp_sym id);
#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif

static inline mrb_int sp_idiv(mrb_int a, mrb_int b) {
  mrb_int q = a / b; mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) q--;
  return q;
}
static inline mrb_int sp_imod(mrb_int a, mrb_int b) {
  mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) r += b;
  return r;
}

static mrb_int sp_gcd(mrb_int a,mrb_int b){if(a<0)a=-a;if(b<0)b=-b;while(b){mrb_int t=b;b=a%b;a=t;}return a;}
static mrb_int sp_int_clamp(mrb_int v,mrb_int lo,mrb_int hi){return v<lo?lo:v>hi?hi:v;}
static inline char *sp_str_alloc_raw(size_t total_with_null);  /* fwd decl */
static const char*sp_int_chr(mrb_int n){char*s=sp_str_alloc_raw(2);s[0]=(char)n;s[1]=0;return s;}
typedef struct{mrb_int first;mrb_int last;}sp_Range;
static sp_Range sp_range_new(mrb_int f,mrb_int l){sp_Range r;r.first=f;r.last=l;return r;}

typedef struct sp_gc_hdr { struct sp_gc_hdr *next; void (*finalize)(void *); void (*scan)(void *); size_t size; unsigned marked : 1; } sp_gc_hdr;
static sp_gc_hdr *sp_gc_heap = NULL; static size_t sp_gc_bytes = 0; static size_t sp_gc_threshold = 256*1024;

/* ---- String GC ---- */
typedef struct sp_str_hdr { struct sp_str_hdr *next; size_t size; } sp_str_hdr;
static sp_str_hdr *sp_str_heap = NULL;
#define SPL(s) (&("\xff" s)[1])
static const char sp_str_empty_data[] = "\xff";
#define sp_str_empty (sp_str_empty_data + 1)

static char *sp_str_alloc(size_t len) {
  size_t total = sizeof(sp_str_hdr) + 1 + len + 1;
  sp_str_hdr *h = (sp_str_hdr *)malloc(total);
  h->next = sp_str_heap;
  h->size = total;
  sp_str_heap = h;
  sp_gc_bytes += total;
  char *body = (char *)(h + 1);
  body[0] = (char)0xfe;
  body[1 + len] = 0;
  return body + 1;
}

static inline char *sp_str_alloc_raw(size_t total_with_null) {
  return sp_str_alloc(total_with_null > 0 ? total_with_null - 1 : 0);
}

static const char *sp_str_dup_external(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *r = sp_str_alloc(n);
  memcpy(r, s, n);
  return r;
}

static inline void sp_mark_string(const char *s) {
  if (!s) return;
  if ((unsigned char)s[-1] == 0xfe) {
    ((char *)s)[-1] = (char)0xfc;
  }
}

static void sp_str_sweep(void) {
  sp_str_hdr **pp = &sp_str_heap;
  while (*pp) {
    sp_str_hdr *h = *pp;
    char *body = (char *)(h + 1);
    if ((unsigned char)body[0] == 0xfc) {
      body[0] = (char)0xfe;
      pp = &h->next;
    } else {
      *pp = h->next;
      sp_gc_bytes -= h->size;
      free(h);
    }
  }
}

#define SP_GC_STACK_MAX 65536
static void **sp_gc_roots[SP_GC_STACK_MAX]; static int sp_gc_nroots = 0;
#define SP_GC_SAVE() int __attribute__((cleanup(sp_gc_cleanup))) _gc_saved = sp_gc_nroots
#define SP_GC_ROOT(v) do{if(sp_gc_nroots<SP_GC_STACK_MAX)sp_gc_roots[sp_gc_nroots++]=(void**)&(v);}while(0)
#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved
#define SP_GC_MARK_STACK_MAX (1024*64)
static void**sp_gc_mark_stack=NULL;static int sp_gc_mark_top=0;
static void sp_gc_mark(void*obj){if(!obj)return;unsigned char pm=((unsigned char*)obj)[-1];if(pm==0xfe){((char*)obj)[-1]=(char)0xfc;return;}if(pm==0xfc||pm==0xff)return;sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->marked)return;h->marked=1;if(h->scan){if(sp_gc_mark_stack&&sp_gc_mark_top<SP_GC_MARK_STACK_MAX){sp_gc_mark_stack[sp_gc_mark_top++]=obj;}else{h->scan(obj);}}}
static void sp_gc_mark_all(void){if(!sp_gc_mark_stack)sp_gc_mark_stack=(void**)malloc(sizeof(void*)*SP_GC_MARK_STACK_MAX);sp_gc_mark_top=0;for(int i=0;i<sp_gc_nroots;i++){void*obj=*sp_gc_roots[i];if(obj)sp_gc_mark(obj);}while(sp_gc_mark_top>0){void*obj=sp_gc_mark_stack[--sp_gc_mark_top];sp_gc_hdr*h=(sp_gc_hdr*)((char*)obj-sizeof(sp_gc_hdr));if(h->scan)h->scan(obj);}}
static void sp_gc_cleanup(int*p){sp_gc_nroots=*p;}
#define SP_GC_NBUCKETS 32
static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];
static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}
static int sp_gc_cycle=0;
static sp_gc_hdr*sp_gc_old_heap=NULL;static size_t sp_gc_old_bytes=0;
#define SP_GC_FULL_INTERVAL 8
static void sp_gc_collect(void){int full=(sp_gc_cycle%SP_GC_FULL_INTERVAL==0);sp_gc_cycle++;sp_gc_hdr*hh=sp_gc_old_heap;while(hh){hh->marked=0;hh=hh->next;}sp_gc_mark_all();if(full){sp_gc_hdr**pp=&sp_gc_old_heap;sp_gc_old_bytes=0;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));free(h);}else{h->marked=1;sp_gc_old_bytes+=h->size;pp=&h->next;}}}else{hh=sp_gc_old_heap;while(hh){hh->marked=1;hh=hh->next;}}sp_gc_hdr**pp=&sp_gc_heap;sp_gc_bytes=sp_gc_old_bytes;while(*pp){sp_gc_hdr*h=*pp;if(!h->marked){*pp=h->next;if(h->finalize)h->finalize((char*)h+sizeof(sp_gc_hdr));free(h);}else{h->marked=1;*pp=h->next;h->next=sp_gc_old_heap;sp_gc_old_heap=h;sp_gc_old_bytes+=h->size;sp_gc_bytes+=h->size;}}sp_str_sweep();malloc_trim(0);}
static size_t sp_gc_threshold_init=256*1024;
void*sp_gc_alloc(size_t sz,void(*fin)(void*),void(*scn)(void*)){if(sp_gc_bytes>sp_gc_threshold){size_t before=sp_gc_bytes;sp_gc_collect();size_t freed=before-sp_gc_bytes;if(freed<before/4){sp_gc_threshold=before*2;}else if(sp_gc_bytes>0){sp_gc_threshold=sp_gc_bytes*4;if(sp_gc_threshold<sp_gc_threshold_init)sp_gc_threshold=sp_gc_threshold_init;}else{sp_gc_threshold=sp_gc_threshold_init;}}size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}
void*sp_gc_alloc_nogc(size_t sz,void(*fin)(void*),void(*scn)(void*)){size_t need=sizeof(sp_gc_hdr)+sz;sp_gc_hdr*h=(sp_gc_hdr*)calloc(1,need);h->finalize=fin;h->scan=scn;h->size=need;h->marked=0;h->next=sp_gc_heap;sp_gc_heap=h;sp_gc_bytes+=need;return(char*)h+sizeof(sp_gc_hdr);}

typedef struct{mrb_int*data;mrb_int start;mrb_int len;mrb_int cap;}sp_IntArray;
static void sp_IntArray_fin(void*p){free(((sp_IntArray*)p)->data);}
static sp_IntArray*sp_IntArray_new(void){sp_IntArray*a=(sp_IntArray*)sp_gc_alloc(sizeof(sp_IntArray),sp_IntArray_fin,NULL);a->cap=16;a->data=(mrb_int*)malloc(sizeof(mrb_int)*a->cap);a->start=0;a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}return a;}
static sp_IntArray*sp_IntArray_from_range(mrb_int s,mrb_int e){sp_IntArray*a=sp_IntArray_new();mrb_int n=e-s+1;if(n<0)n=0;if(n>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=n;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}for(mrb_int i=0;i<n;i++)a->data[i]=s+i;a->len=n;return a;}
static sp_IntArray*sp_IntArray_dup(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();if(a->len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=a->len;b->data=(mrb_int*)realloc(b->data,sizeof(mrb_int)*b->cap);h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start,sizeof(mrb_int)*a->len);b->len=a->len;return b;}
static void __attribute__((noinline)) sp_IntArray_push_grow(sp_IntArray*a){if(a->start>0){memmove(a->data,a->data+a->start,sizeof(mrb_int)*a->len);a->start=0;if(a->len<a->cap)return;}{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}}
static inline void sp_IntArray_push(sp_IntArray*a,mrb_int v){if(a->start+a->len>=a->cap)sp_IntArray_push_grow(a);a->data[a->start+a->len]=v;a->len++;}
static inline mrb_int sp_IntArray_pop(sp_IntArray*a){return a->data[a->start+--a->len];}
static inline mrb_int sp_IntArray_shift(sp_IntArray*a){mrb_int v=a->data[a->start];a->start++;a->len--;return v;}
static inline mrb_int sp_IntArray_length(sp_IntArray*a){return a->len;}
static inline mrb_bool sp_IntArray_empty(sp_IntArray*a){return a->len==0;}
static inline mrb_int sp_IntArray_get(sp_IntArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[a->start+i];}
static void sp_IntArray_set_slow(sp_IntArray*a,mrb_int i,mrb_int v){while(a->start+i>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}while(i>=a->len){a->data[a->start+a->len]=0;a->len++;}a->data[a->start+i]=v;}
static inline void sp_IntArray_set(sp_IntArray*a,mrb_int i,mrb_int v){if(i<0)i+=a->len;if(i>=0&&i<a->len){a->data[a->start+i]=v;return;}sp_IntArray_set_slow(a,i,v);}
static void sp_IntArray_reverse_bang(sp_IntArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
static int _sp_int_cmp(const void*a,const void*b){mrb_int va=*(const mrb_int*)a,vb=*(const mrb_int*)b;return(va>vb)-(va<vb);}
static sp_IntArray*sp_IntArray_sort(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);qsort(b->data+b->start,b->len,sizeof(mrb_int),_sp_int_cmp);return b;}
static void sp_IntArray_sort_bang(sp_IntArray*a){qsort(a->data+a->start,a->len,sizeof(mrb_int),_sp_int_cmp);}
static mrb_int sp_IntArray_min(sp_IntArray*a){mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]<m)m=a->data[a->start+i];return m;}
static mrb_int sp_IntArray_max(sp_IntArray*a){mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]>m)m=a->data[a->start+i];return m;}
static mrb_int sp_IntArray_sum(sp_IntArray*a){mrb_int s=0;for(mrb_int i=0;i<a->len;i++)s+=a->data[a->start+i];return s;}
static mrb_bool sp_IntArray_include(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return TRUE;return FALSE;}
static mrb_int sp_IntArray_index(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return i;return -1;}
static mrb_int sp_IntArray_rindex(sp_IntArray*a,mrb_int v){for(mrb_int i=a->len-1;i>=0;i--)if(a->data[a->start+i]==v)return i;return -1;}
static mrb_int sp_IntArray_delete_at(sp_IntArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return 0;mrb_int v=a->data[a->start+i];for(mrb_int j=i;j<a->len-1;j++)a->data[a->start+j]=a->data[a->start+j+1];a->len--;return v;}
static mrb_int sp_IntArray_delete(sp_IntArray*a,mrb_int v){mrb_int w=0;for(mrb_int i=0;i<a->len;i++){if(a->data[a->start+i]!=v){a->data[a->start+w]=a->data[a->start+i];w++;}}mrb_int d=a->len-w;a->len=w;return d>0?v:0;}
static void sp_IntArray_insert(sp_IntArray*a,mrb_int i,mrb_int v){if(i<0)i+=a->len+1;sp_IntArray_push(a,0);for(mrb_int j=a->len-1;j>i;j--)a->data[a->start+j]=a->data[a->start+j-1];a->data[a->start+i]=v;}
static sp_IntArray*sp_int_digits(mrb_int n,mrb_int base){sp_IntArray*a=sp_IntArray_new();if(base<2)base=10;if(n==0){sp_IntArray_push(a,0);return a;}if(n<0)n=-n;while(n>0){sp_IntArray_push(a,n%base);n/=base;}return a;}
static sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){int found=0;for(mrb_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}return b;}
static void sp_IntArray_unshift(sp_IntArray*a,mrb_int v){if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}else{mrb_int e=a->len+1;if(e>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}memmove(a->data+1,a->data,sizeof(mrb_int)*a->len);a->data[0]=v;a->len++;}}
static const char*sp_IntArray_join(sp_IntArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}char tmp[32];int n=snprintf(tmp,32,"%lld",(long long)a->data[a->start+i]);if(len+n>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,tmp,n);len+=n;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_IntArray_eq(sp_IntArray*a,sp_IntArray*b){if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]!=b->data[b->start+i])return FALSE;return TRUE;}

typedef struct{mrb_float*data;mrb_int len;mrb_int cap;}sp_FloatArray;
static void sp_FloatArray_fin(void*p){sp_FloatArray*a=(sp_FloatArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;free(a->data);}
static sp_FloatArray*sp_FloatArray_new(void){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);a->cap=16;a->data=(mrb_float*)malloc(sizeof(mrb_float)*a->cap);a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}return a;}
static inline void sp_FloatArray_push(sp_FloatArray*a,mrb_float v){if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*a->cap;h->size-=sizeof(mrb_float)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);h->size+=sizeof(mrb_float)*a->cap;sp_gc_bytes+=sizeof(mrb_float)*a->cap;}a->data[a->len++]=v;}
static mrb_float sp_FloatArray_min(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]<m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_max(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]>m)m=a->data[i];return m;}
static mrb_float sp_FloatArray_sum(sp_FloatArray*a){mrb_float s=0;for(mrb_int i=0;i<a->len;i++)s+=a->data[i];return s;}
static inline mrb_float sp_FloatArray_pop(sp_FloatArray*a){return a->data[--a->len];}
static inline mrb_int sp_FloatArray_length(sp_FloatArray*a){return a->len;}
static inline mrb_bool sp_FloatArray_empty(sp_FloatArray*a){return a->len==0;}
static inline mrb_float sp_FloatArray_get(sp_FloatArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}
static inline void sp_FloatArray_set(sp_FloatArray*a,mrb_int i,mrb_float v){if(i<0)i+=a->len;while(i>=a->cap){a->cap=a->cap*2+1;a->data=(mrb_float*)realloc(a->data,sizeof(mrb_float)*a->cap);}while(i>=a->len){a->data[a->len]=0.0;a->len++;}a->data[i]=v;}

/* ---- PtrArray: array of void* pointers ---- */
typedef struct{void**data;mrb_int len;mrb_int cap;void(*scan_elem)(void*);}sp_PtrArray;
static void sp_PtrArray_fin(void*p){sp_PtrArray*a=(sp_PtrArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(void*)*a->cap;h->size-=sizeof(void*)*a->cap;free(a->data);}
static void sp_PtrArray_gc_scan(void*p){sp_PtrArray*a=(sp_PtrArray*)p;if(!a->scan_elem)return;for(mrb_int i=0;i<a->len;i++){if(a->data[i])a->scan_elem(a->data[i]);}}
static sp_PtrArray*sp_PtrArray_new_scan(void(*scan_elem)(void*)){sp_PtrArray*a=(sp_PtrArray*)sp_gc_alloc(sizeof(sp_PtrArray),sp_PtrArray_fin,scan_elem?sp_PtrArray_gc_scan:NULL);a->cap=16;a->data=(void**)malloc(sizeof(void*)*a->cap);a->len=0;a->scan_elem=scan_elem;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(void*)*a->cap;sp_gc_bytes+=sizeof(void*)*a->cap;}return a;}
static sp_PtrArray*sp_PtrArray_new(void){return sp_PtrArray_new_scan(sp_gc_mark);}
static inline void sp_PtrArray_push(sp_PtrArray*a,void*v){if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(void*)*a->cap;h->size-=sizeof(void*)*a->cap;a->cap=a->cap*2+1;a->data=(void**)realloc(a->data,sizeof(void*)*a->cap);h->size+=sizeof(void*)*a->cap;sp_gc_bytes+=sizeof(void*)*a->cap;}a->data[a->len++]=v;}
static inline void*sp_PtrArray_get(sp_PtrArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}
static inline void sp_PtrArray_set(sp_PtrArray*a,mrb_int i,void*v){if(i<0)i+=a->len;a->data[i]=v;}
static inline mrb_int sp_PtrArray_length(sp_PtrArray*a){return a->len;}
static inline mrb_bool sp_PtrArray_empty(sp_PtrArray*a){return a->len==0;}

typedef struct{const char**data;mrb_int len;mrb_int cap;}sp_StrArray;
static void sp_StrArray_fin(void*p){sp_StrArray*a=(sp_StrArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;free(a->data);}
static void sp_StrArray_scan(void*p){sp_StrArray*a=(sp_StrArray*)p;for(mrb_int i=0;i<a->len;i++)sp_mark_string(a->data[i]);}
static sp_StrArray*sp_StrArray_new(void){sp_StrArray*a=(sp_StrArray*)sp_gc_alloc(sizeof(sp_StrArray),sp_StrArray_fin,sp_StrArray_scan);a->cap=16;a->data=(const char**)malloc(sizeof(const char*)*a->cap);a->len=0;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(const char*)*a->cap;sp_gc_bytes+=sizeof(const char*)*a->cap;}return a;}
static inline void sp_StrArray_push(sp_StrArray*a,const char*v){if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(const char*)*a->cap;h->size-=sizeof(const char*)*a->cap;a->cap=a->cap*2+1;a->data=(const char**)realloc(a->data,sizeof(const char*)*a->cap);h->size+=sizeof(const char*)*a->cap;sp_gc_bytes+=sizeof(const char*)*a->cap;}a->data[a->len++]=v;}
static const char*sp_StrArray_pop(sp_StrArray*a){return a->data[--a->len];}
static inline mrb_int sp_StrArray_length(sp_StrArray*a){return a->len;}
static inline mrb_bool sp_StrArray_empty(sp_StrArray*a){return a->len==0;}
static inline const char*sp_StrArray_get(sp_StrArray*a,mrb_int i){if(i<0)i+=a->len;return a->data[i];}
static inline void sp_StrArray_set(sp_StrArray*a,mrb_int i,const char*v){if(i<0)i+=a->len;while(i>=a->len)sp_StrArray_push(a,"");a->data[i]=v;}
static const char*sp_StrArray_join(sp_StrArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}size_t el=strlen(a->data[i]);if(len+el>=cap){cap=(len+el)*2+1;buf=(char*)realloc(buf,cap);}memcpy(buf+len,a->data[i],el);len+=el;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
static mrb_bool sp_StrArray_include(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return TRUE;return FALSE;}
static mrb_int sp_StrArray_index(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return i;return -1;}
static mrb_int sp_StrArray_rindex(sp_StrArray*a,const char*v){for(mrb_int i=a->len-1;i>=0;i--)if(strcmp(a->data[i],v)==0)return i;return -1;}
static sp_StrArray*sp_StrArray_compact(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=NULL)sp_StrArray_push(r,a->data[i]);return r;}
static const char*sp_StrArray_delete_at(sp_StrArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;const char*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
static const char*sp_StrArray_delete(sp_StrArray*a,const char*v){mrb_int w=0;const char*found=NULL;for(mrb_int i=0;i<a->len;i++){if(strcmp(a->data[i],v)!=0){a->data[w]=a->data[i];w++;}else{found=a->data[i];}}a->len=w;return found;}
static void sp_StrArray_insert(sp_StrArray*a,mrb_int i,const char*v){if(i<0)i+=a->len+1;sp_StrArray_push(a,"");for(mrb_int j=a->len-1;j>i;j--)a->data[j]=a->data[j-1];a->data[i]=v;}

static inline uint64_t sp_str_hash(const char*s){uint64_t h=14695981039346656037ULL;while(*s){h^=(unsigned char)*s++;h*=1099511628211ULL;}return h;}
typedef struct{const char**keys;mrb_int*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;}sp_StrIntHash;
static void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrIntHash_scan(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i])sp_mark_string(h->keys[i]);}}
static sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,sp_StrIntHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;return h;}
static void sp_StrIntHash_grow(sp_StrIntHash*h){mrb_int oc=h->cap;const char**ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);mrb_int ol=h->len;h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static mrb_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return 0;}
static void sp_StrIntHash_set(sp_StrIntHash*h,const char*k,mrb_int v){if(h->len*2>=h->cap)sp_StrIntHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_StrIntHash_length(sp_StrIntHash*h){return h->len;}
static void sp_StrIntHash_delete(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=0;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrIntHash_keys(sp_StrIntHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_IntArray*sp_StrIntHash_values(sp_StrIntHash*h){sp_IntArray*a=sp_IntArray_new();for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,sp_StrIntHash_get(h,h->order[i]));return a;}
static sp_StrIntHash*sp_StrArray_tally(sp_StrArray*a){sp_StrIntHash*h=sp_StrIntHash_new();for(mrb_int i=0;i<a->len;i++){const char*k=a->data[i];mrb_int c=sp_StrIntHash_has_key(h,k)?sp_StrIntHash_get(h,k):0;sp_StrIntHash_set(h,k,c+1);}return h;}
static sp_StrIntHash*sp_StrIntHash_merge(sp_StrIntHash*a,sp_StrIntHash*b){sp_StrIntHash*r=sp_StrIntHash_new();for(mrb_int i=0;i<a->len;i++)sp_StrIntHash_set(r,a->order[i],sp_StrIntHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(r,b->order[i],sp_StrIntHash_get(b,b->order[i]));return r;}
static void sp_StrIntHash_update(sp_StrIntHash*a,sp_StrIntHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(a,b->order[i],sp_StrIntHash_get(b,b->order[i]));}

typedef struct{const char**keys;const char**vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;}sp_StrStrHash;
static void sp_StrStrHash_fin(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrStrHash_scan(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_string(h->vals[i]);}}}
static sp_StrStrHash*sp_StrStrHash_new(void){sp_StrStrHash*h=(sp_StrStrHash*)sp_gc_alloc(sizeof(sp_StrStrHash),sp_StrStrHash_fin,sp_StrStrHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;return h;}
static void sp_StrStrHash_grow(sp_StrStrHash*h){mrb_int oc=h->cap;const char**ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static const char*sp_StrStrHash_get(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return"";}
static void sp_StrStrHash_set(sp_StrStrHash*h,const char*k,const char*v){if(h->len*2>=h->cap)sp_StrStrHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrStrHash_has_key(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_StrStrHash_length(sp_StrStrHash*h){return h->len;}
static void sp_StrStrHash_delete(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->keys[idx]=NULL;h->vals[idx]=NULL;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrStrHash_keys(sp_StrStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_StrStrHash_values(sp_StrStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_StrStrHash_get(h,h->order[i]));return a;}
static sp_StrStrHash*sp_StrStrHash_invert(sp_StrStrHash*h){sp_StrStrHash*r=sp_StrStrHash_new();for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrStrHash_set(r,sp_StrStrHash_get(h,k),k);}return r;}
static void sp_StrStrHash_update(sp_StrStrHash*a,sp_StrStrHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrStrHash_set(a,b->order[i],sp_StrStrHash_get(b,b->order[i]));}

typedef struct{mrb_int*keys;const char**vals;mrb_int*order;mrb_bool*used;mrb_int len;mrb_int cap;mrb_int mask;}sp_IntStrHash;
static void sp_IntStrHash_fin(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
static void sp_IntStrHash_scan(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;for(mrb_int i=0;i<h->cap;i++)if(h->used[i])sp_mark_string(h->vals[i]);}
static sp_IntStrHash*sp_IntStrHash_new(void){sp_IntStrHash*h=(sp_IntStrHash*)sp_gc_alloc(sizeof(sp_IntStrHash),sp_IntStrHash_fin,sp_IntStrHash_scan);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;return h;}
static inline mrb_int _sp_istr_idx(mrb_int mask,mrb_int k){return(mrb_int)(((uint64_t)(unsigned long long)k*11400714819323198485ULL)&(uint64_t)mask);}
static void sp_IntStrHash_grow(sp_IntStrHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;const char**ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];const char*v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntStrHash_set(sp_IntStrHash*h,mrb_int k,const char*v){if(h->len*2>=h->cap)sp_IntStrHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static const char*sp_IntStrHash_get(sp_IntStrHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return"";}
static mrb_bool sp_IntStrHash_has_key(sp_IntStrHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_IntStrHash_length(sp_IntStrHash*h){return h->len;}
static sp_IntArray*sp_IntStrHash_keys(sp_IntStrHash*h){sp_IntArray*a=sp_IntArray_new();for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_IntStrHash_values(sp_IntStrHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_IntStrHash_get(h,h->order[i]));return a;}

/* Reuse an existing StrArray for split, avoiding GC alloc.
   Clears a->len and refills.  Substring strings are still malloc'd. */
static void sp_str_split_into(sp_StrArray*a,const char*s,const char*sep){
  a->len=0;if(*s==0)return;size_t sl=strlen(sep);
  if(sl==0){for(size_t i=0;s[i];i++){char*c=sp_str_alloc_raw(2);c[0]=s[i];c[1]=0;sp_StrArray_push(a,c);}return;}
  const char*p=s;while(1){const char*f=strstr(p,sep);if(!f){char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);break;}
  size_t n=f-p;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);p=f+sl;}}
/* Extract the n-th field (0-based) from s split by sep, without
   allocating a full StrArray.  Returns a newly allocated string.
   If the field doesn't exist, returns "". */
static const char*sp_str_field(const char*s,const char*sep,mrb_int n){
  size_t sl=strlen(sep);mrb_int cur=0;const char*p=s;
  if(sl==0)return("");
  while(cur<n){const char*f=strstr(p,sep);if(!f)return("");p=f+sl;cur++;}
  const char*end=strstr(p,sep);size_t len=end?((size_t)(end-p)):strlen(p);
  char*r=sp_str_alloc_raw(len+1);memcpy(r,p,len);r[len]=0;return r;}
/* Count fields in s split by sep (without allocating). */
static mrb_int sp_str_field_count(const char*s,const char*sep){
  if(*s==0)return 0;size_t sl=strlen(sep);if(sl==0)return(mrb_int)strlen(s);
  mrb_int c=1;const char*p=s;while((p=strstr(p,sep))!=NULL){c++;p+=sl;}return c;}
static const char*sp_str_concat(const char*a,const char*b){size_t la=strlen(a),lb=strlen(b);char*r=sp_str_alloc(la+lb);memcpy(r,a,la);memcpy(r+la,b,lb);return r;}
static const char*sp_str_concat3(const char*a,const char*b,const char*c){size_t la=strlen(a),lb=strlen(b),lc=strlen(c);char*r=sp_str_alloc(la+lb+lc);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);return r;}
static const char*sp_str_concat4(const char*a,const char*b,const char*c,const char*d){size_t la=strlen(a),lb=strlen(b),lc=strlen(c),ld=strlen(d);char*r=sp_str_alloc(la+lb+lc+ld);memcpy(r,a,la);memcpy(r+la,b,lb);memcpy(r+la+lb,c,lc);memcpy(r+la+lb+lc,d,ld);return r;}
/* Concatenate N strings into a single GC-managed buffer. */
static const char*sp_str_concat_arr(const char *const *parts,int n){size_t total=0;for(int i=0;i<n;i++)total+=strlen(parts[i]);char*r=sp_str_alloc(total);char*p=r;for(int i=0;i<n;i++){size_t sl=strlen(parts[i]);memcpy(p,parts[i],sl);p+=sl;}return r;}
static const char*sp_int_to_s(mrb_int n){char*b=sp_str_alloc_raw(32);snprintf(b,32,"%lld",(long long)n);return b;}
static const char*sp_int_to_s_base(mrb_int n,mrb_int base){if(base<2||base>36)base=10;char*b=sp_str_alloc_raw(72);char tmp[72];int i=0;int neg=0;uint64_t u;if(n<0){neg=1;u=(uint64_t)(-(n+1))+1;}else{u=(uint64_t)n;}if(u==0){tmp[i++]='0';}else{while(u>0){mrb_int d=u%base;tmp[i++]=d<10?'0'+d:'a'+d-10;u/=base;}}int j=0;if(neg)b[j++]='-';while(i>0)b[j++]=tmp[--i];b[j]=0;return b;}
static const char*sp_float_to_s(mrb_float f){char*b=sp_str_alloc_raw(64);snprintf(b,64,"%g",f);return b;}
static const char*sp_str_upcase(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++)r[i]=toupper((unsigned char)s[i]);return r;}
static const char*sp_str_downcase(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);return r;}
static const char*sp_str_swapcase(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++){unsigned char c=(unsigned char)s[i];if(isupper(c))r[i]=tolower(c);else if(islower(c))r[i]=toupper(c);else r[i]=s[i];}return r;}
static const char*sp_str_delete_prefix(const char*s,const char*p){size_t sl=strlen(s),pl=strlen(p);if(pl<=sl&&memcmp(s,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s+pl,sl-pl+1);return r;}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);return r;}
static const char*sp_str_substr(const char*s,mrb_int start,mrb_int len){if(len<=0){char*r=sp_str_alloc_raw(1);r[0]=0;return r;}char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;return r;}
static const char*sp_str_delete_suffix(const char*s,const char*p){size_t sl=strlen(s),pl=strlen(p);if(pl<=sl&&memcmp(s+sl-pl,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s,sl-pl);r[sl-pl]=0;return r;}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);return r;}
static const char*sp_str_succ(const char*s){size_t l=strlen(s);if(l==0){char*r=sp_str_alloc_raw(1);r[0]=0;return r;}char*r=sp_str_alloc_raw(l+2);memcpy(r,s,l+1);mrb_int i=(mrb_int)l-1;while(i>=0){unsigned char c=(unsigned char)r[i];if(c>='0'&&c<'9'){r[i]=c+1;return r;}if(c=='9'){r[i]='0';i--;continue;}if(c>='a'&&c<'z'){r[i]=c+1;return r;}if(c=='z'){r[i]='a';i--;continue;}if(c>='A'&&c<'Z'){r[i]=c+1;return r;}if(c=='Z'){r[i]='A';i--;continue;}r[i]=c+1;return r;}memmove(r+1,r,l+1);if(r[1]=='0')r[0]='1';else if(r[1]=='a')r[0]='a';else if(r[1]=='A')r[0]='A';else r[0]=r[1];return r;}
static const char*sp_gets(void){char buf[4096];if(!fgets(buf,sizeof(buf),stdin))return NULL;size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);return r;}
static sp_StrArray*sp_readlines(void){sp_StrArray*a=sp_StrArray_new();char buf[4096];while(fgets(buf,sizeof(buf),stdin)){size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);sp_StrArray_push(a,r);}return a;}
static const char*sp_str_strip(const char*s){while(*s&&isspace((unsigned char)*s))s++;size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static const char*sp_str_chomp(const char*s){size_t l=strlen(s);while(l>0&&(s[l-1]=='\n'||s[l-1]=='\r'))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static mrb_bool sp_str_include(const char*s,const char*sub){return strstr(s,sub)!=NULL;}
static mrb_bool sp_str_start_with(const char*s,const char*p){return strncmp(s,p,strlen(p))==0;}
static mrb_bool sp_str_end_with(const char*s,const char*suf){size_t ls=strlen(s),lsuf=strlen(suf);if(lsuf>ls)return FALSE;return strcmp(s+ls-lsuf,suf)==0;}
static sp_StrArray*sp_str_split(const char*s,const char*sep){sp_StrArray*a=sp_StrArray_new();if(*s==0)return a;size_t sl=strlen(sep);if(sl==0){for(size_t i=0;s[i];i++){char*c=sp_str_alloc_raw(2);c[0]=s[i];c[1]=0;sp_StrArray_push(a,c);}return a;}const char*p=s;while(1){const char*f=strstr(p,sep);if(!f){char*r=sp_str_alloc_raw(strlen(p)+1);strcpy(r,p);sp_StrArray_push(a,r);break;}size_t n=f-p;char*r=sp_str_alloc_raw(n+1);memcpy(r,p,n);r[n]=0;sp_StrArray_push(a,r);p=f+sl;}return a;}
static const char*sp_str_gsub(const char*s,const char*pat,const char*rep){size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);if(pl==0)return s;size_t cap=sl*2+1;char*out=(char*)malloc(cap);size_t ol=0;const char*p=s;while(*p){const char*f=strstr(p,pat);if(!f){size_t n=strlen(p);if(ol+n>=cap){cap=(ol+n)*2+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;break;}size_t n=f-p;if(ol+n+rl>=cap){cap=(ol+n+rl)*2+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;memcpy(out+ol,rep,rl);ol+=rl;p=f+pl;}out[ol]=0;return out;}
static mrb_int sp_str_index(const char*s,const char*sub){const char*f=strstr(s,sub);if(!f)return -1;return(mrb_int)(f-s);}
static char sp_char_cache[256][3];
static int sp_char_cache_init = 0;
static const char*sp_str_sub_range(const char*s,mrb_int start,mrb_int len){mrb_int sl=(mrb_int)strlen(s);if(start<0)start+=sl;if(start<0)start=0;if(start>=sl)return &("\xff" "")[1];if(len<0)len=0;if(start+len>sl)len=sl-start;if(len==1){unsigned char c=(unsigned char)s[start];if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;return r;}
/* Same as sp_str_sub_range but caller provides the string length
   (useful when strlen has been hoisted out of a loop). */
static const char*sp_str_sub_range_len(const char*s,mrb_int sl,mrb_int start,mrb_int len){if(start<0)start+=sl;if(start<0)start=0;if(start>=sl)return &("\xff" "")[1];if(len<0)len=0;if(start+len>sl)len=sl-start;if(len==1){unsigned char c=(unsigned char)s[start];if(!sp_char_cache_init){for(int i=0;i<256;i++){sp_char_cache[i][0]=(char)0xff;sp_char_cache[i][1]=(char)i;sp_char_cache[i][2]=0;}sp_char_cache_init=1;}return &sp_char_cache[c][1];}char*r=sp_str_alloc_raw(len+1);memcpy(r,s+start,len);r[len]=0;return r;}
static const char*sp_sprintf(const char*fmt,...){char*b=sp_str_alloc_raw(4096);va_list ap;va_start(ap,fmt);vsnprintf(b,4096,fmt,ap);va_end(ap);return b;}
static const char*sp_str_reverse(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++)r[i]=s[l-1-i];r[l]=0;return r;}
static const char*sp_str_sub(const char*s,const char*pat,const char*rep){const char*f=strstr(s,pat);if(!f)return s;size_t pl=strlen(pat),rl=strlen(rep),sl=strlen(s);char*r=sp_str_alloc_raw(sl-pl+rl+1);size_t n=f-s;memcpy(r,s,n);memcpy(r+n,rep,rl);memcpy(r+n+rl,f+pl,sl-n-pl+1);return r;}
static const char*sp_str_capitalize(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<=l;i++)r[i]=tolower((unsigned char)s[i]);if(l>0)r[0]=toupper((unsigned char)r[0]);return r;}
static mrb_int sp_str_count(const char*s,const char*chars){mrb_int c=0;for(size_t i=0;s[i];i++){for(size_t j=0;chars[j];j++){if(s[i]==chars[j]){c++;break;}}}return c;}
static const char*sp_str_repeat(const char*s,mrb_int n){if(n<=0)return"";size_t l=strlen(s);char*r=sp_str_alloc_raw(l*n+1);for(mrb_int i=0;i<n;i++)memcpy(r+l*i,s,l);r[l*n]=0;return r;}
static sp_IntArray*sp_str_bytes(const char*s){sp_IntArray*a=sp_IntArray_new();for(size_t i=0;s[i];i++)sp_IntArray_push(a,(mrb_int)(unsigned char)s[i]);return a;}
static const char*sp_str_tr(const char*s,const char*from,const char*to){size_t l=strlen(s),fl=strlen(from),tl=strlen(to);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){r[i]=s[i];for(size_t j=0;j<fl;j++){if(s[i]==from[j]){if(j<tl)r[i]=to[j];else if(tl>0)r[i]=to[tl-1];break;}}}r[l]=0;return r;}
static const char*sp_str_delete(const char*s,const char*chars){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);size_t n=0;for(size_t i=0;i<l;i++){int found=0;for(size_t j=0;chars[j];j++){if(s[i]==chars[j]){found=1;break;}}if(!found)r[n++]=s[i];}r[n]=0;return r;}
static const char*sp_str_squeeze(const char*s){size_t l=strlen(s);char*r=sp_str_alloc_raw(l+1);size_t n=0;for(size_t i=0;i<l;i++){if(i==0||s[i]!=s[i-1])r[n++]=s[i];}r[n]=0;return r;}
static const char*sp_str_ljust(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=sp_str_alloc_raw(w+1);memcpy(r,s,l);memset(r+l,' ',w-l);r[w]=0;return r;}
static const char*sp_str_rjust(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=sp_str_alloc_raw(w+1);memset(r,' ',w-l);memcpy(r+w-l,s,l+1);return r;}
static const char*sp_str_center(const char*s,mrb_int w){size_t l=strlen(s);if((mrb_int)l>=w)return s;mrb_int pad=w-l;mrb_int left=pad/2;mrb_int right=pad-left;char*r=sp_str_alloc_raw(w+1);memset(r,' ',left);memcpy(r+left,s,l);memset(r+left+l,' ',right);r[w]=0;return r;}
static const char*sp_str_ljust2(const char*s,mrb_int w,const char*pad){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=sp_str_alloc_raw(w+1);memcpy(r,s,l);char pc=pad[0];for(mrb_int i=l;i<w;i++)r[i]=pc;r[w]=0;return r;}
static const char*sp_str_rjust2(const char*s,mrb_int w,const char*pad){size_t l=strlen(s);if((mrb_int)l>=w)return s;char*r=sp_str_alloc_raw(w+1);char pc=pad[0];for(mrb_int i=0;i<w-(mrb_int)l;i++)r[i]=pc;memcpy(r+w-l,s,l+1);return r;}
static const char*sp_str_lstrip(const char*s){while(*s&&isspace((unsigned char)*s))s++;char*r=sp_str_alloc_raw(strlen(s)+1);strcpy(r,s);return r;}
static const char*sp_str_rstrip(const char*s){size_t l=strlen(s);while(l>0&&isspace((unsigned char)s[l-1]))l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;return r;}
static const char*sp_str_dup(const char*s){char*r=sp_str_alloc_raw(strlen(s)+1);strcpy(r,s);return r;}

typedef struct{char*data;int64_t len;int64_t cap;}sp_String;
static void sp_String_fin(void*p){free(((sp_String*)p)->data);}
static sp_String*sp_String_new(const char*s){sp_String*r=(sp_String*)sp_gc_alloc(sizeof(sp_String),sp_String_fin,NULL);r->len=(int64_t)strlen(s);r->cap=r->len*2+16;r->data=(char*)malloc(r->cap+1);memcpy(r->data,s,r->len+1);{sp_gc_hdr*h=(sp_gc_hdr*)((char*)r-sizeof(sp_gc_hdr));h->size+=r->cap+1;sp_gc_bytes+=r->cap+1;}return r;}
static inline void sp_String_append(sp_String*s,const char*t){int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));sp_gc_bytes-=s->cap+1;h->size-=s->cap+1;s->cap=(s->len+tl)*2+16;s->data=(char*)realloc(s->data,s->cap+1);h->size+=s->cap+1;sp_gc_bytes+=s->cap+1;}memcpy(s->data+s->len,t,tl+1);s->len+=tl;}
static inline void sp_String_prepend(sp_String*s,const char*t){int64_t tl=(int64_t)strlen(t);if(s->len+tl>=s->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)s-sizeof(sp_gc_hdr));sp_gc_bytes-=s->cap+1;h->size-=s->cap+1;s->cap=(s->len+tl)*2+16;s->data=(char*)realloc(s->data,s->cap+1);h->size+=s->cap+1;sp_gc_bytes+=s->cap+1;}memmove(s->data+tl,s->data,s->len+1);memcpy(s->data,t,tl);s->len+=tl;}
static inline const char*sp_String_cstr(sp_String*s){return s->data;}
static inline int64_t sp_String_length(sp_String*s){return s->len;}
static sp_String*sp_String_dup(sp_String*s){return sp_String_new(s->data);}

/* Regexp engine (link with libspre.a from lib/regexp/) */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
mrb_regexp_pattern* re_compile(const char *pattern, int64_t len, uint32_t flags);
void re_free(mrb_regexp_pattern *pat);
int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size);

/* Regexp globals: $1-$9 captures */
static const char *sp_re_captures[10] = {0};
static int sp_re_caps[64];
static const char *sp_re_last_str = "";

static void sp_re_set_captures(const char *str, int *caps, int ncaps) {
  sp_re_last_str = str;
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  for (int i = 1; i < ncaps && i < 10; i++) {
    if (caps[i*2] >= 0 && caps[i*2+1] >= 0) {
      int len = caps[i*2+1] - caps[i*2];
      char *buf = sp_str_alloc_raw(len+1);
      memcpy(buf, str+caps[i*2], len); buf[len] = 0;
      sp_re_captures[i] = buf;
    }
  }
}

static mrb_int sp_re_match(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int ncaps = 32;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);
  if (n > 0) { sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0] + 1; }
  return 0;
}

static mrb_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, 0, caps, 2) > 0;
}

static const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  size_t cap = slen * 2 + 64; char *out = sp_str_alloc_raw(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    if (olen+before+rlen >= cap) { cap = (olen+before+rlen)*2+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, before); olen += before;
    memcpy(out+olen, rep, rlen); olen += rlen;
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  size_t rest = slen - pos;
  if (olen+rest >= cap) { cap = olen+rest+1; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+pos, rest); olen += rest;
  out[olen] = 0; return out;
}

static const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) return str;
  size_t out_len = caps[0] + rlen + (slen - caps[1]);
  char *out = sp_str_alloc_raw(out_len + 1);
  memcpy(out, str, caps[0]);
  memcpy(out+caps[0], rep, rlen);
  memcpy(out+caps[0]+rlen, str+caps[1], slen-caps[1]+1);
  return out;
}

static sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    int len = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(len+1); memcpy(m, str+caps[0], len); m[len] = 0;
    sp_StrArray_push(arr, m);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  return arr;
}

static sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) {
      int len = slen - pos; char *m = sp_str_alloc_raw(len+1);
      memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m); break;
    }
    int len = caps[0] - pos; char *m = sp_str_alloc_raw(len+1);
    memcpy(m, str+pos, len); m[len] = 0; sp_StrArray_push(arr, m);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  return arr;
}

/* NaN-boxed polymorphic value */
typedef uint64_t sp_RbValue;
#define SP_TAG_INT  0
#define SP_TAG_STR  1
#define SP_TAG_FLT  2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL  4
#define SP_TAG_OBJ  5
#define SP_TAG_SYM  6
typedef struct { int tag; union { mrb_int i; const char *s; mrb_float f; mrb_bool b; void *p; int cls_id; } v; } sp_RbVal;
static sp_RbVal sp_box_int(mrb_int v) { sp_RbVal r; r.tag = SP_TAG_INT; r.v.i = v; return r; }
static sp_RbVal sp_box_str(const char *v) { sp_RbVal r; r.tag = SP_TAG_STR; r.v.s = v; return r; }
static sp_RbVal sp_box_float(mrb_float v) { sp_RbVal r; r.tag = SP_TAG_FLT; r.v.f = v; return r; }
static sp_RbVal sp_box_bool(mrb_bool v) { sp_RbVal r; r.tag = SP_TAG_BOOL; r.v.b = v; return r; }
static sp_RbVal sp_box_nil(void) { sp_RbVal r; r.tag = SP_TAG_NIL; r.v.i = 0; return r; }
static sp_RbVal sp_box_obj(void *p, int cls_id) { sp_RbVal r; r.tag = SP_TAG_OBJ; r.v.p = p; r.v.cls_id = cls_id; return r; }
static sp_RbVal sp_box_sym(sp_sym v) { sp_RbVal r; r.tag = SP_TAG_SYM; r.v.i = (mrb_int)v; return r; }
static void sp_poly_puts(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '\n') putchar('\n'); } else putchar('\n'); break;
    case SP_TAG_FLT: { char _fb[64]; snprintf(_fb,64,"%g",v.v.f); if(!strchr(_fb,'.')&&!strchr(_fb,'e')&&!strchr(_fb,'i')&&!strchr(_fb,'n')){strcat(_fb,".0");} printf("%s\n",_fb); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    default: printf("%lld\n", (long long)v.v.i); break;
  }
}
static mrb_bool sp_poly_nil_p(sp_RbVal v) { return v.tag == SP_TAG_NIL; }
static const char *sp_poly_to_s(sp_RbVal v) { switch (v.tag) { case SP_TAG_INT: { char *b = sp_str_alloc_raw(32); snprintf(b, 32, "%lld", (long long)v.v.i); return b; } case SP_TAG_STR: return v.v.s ? v.v.s : sp_str_empty; case SP_TAG_FLT: { char *b = sp_str_alloc_raw(64); snprintf(b, 64, "%g", v.v.f); return b; } case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false"); case SP_TAG_NIL: return sp_str_empty; case SP_TAG_SYM: return sp_sym_to_s((sp_sym)v.v.i); default: return sp_str_empty; } }
static sp_RbVal sp_poly_add(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i + b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i + b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (mrb_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) return sp_box_str(sp_str_concat(a.v.s, b.v.s)); return sp_box_int(0); }
static sp_RbVal sp_poly_sub(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i - b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); return sp_box_int(0); }
static sp_RbVal sp_poly_mul(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return sp_box_int(a.v.i * b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i * b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (mrb_float)b.v.i); return sp_box_int(0); }
static mrb_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return a.v.i > b.v.i; if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return a.v.f > b.v.f; return FALSE; }

/* PolyArray: array of sp_RbVal */
typedef struct { sp_RbVal *data; mrb_int len; mrb_int cap; } sp_PolyArray;
static sp_PolyArray *sp_PolyArray_new(void) { sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), NULL, NULL); a->cap = 16; a->data = (sp_RbVal *)malloc(sizeof(sp_RbVal) * a->cap); a->len = 0; return a; }
static void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v) { if (a->len >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (sp_RbVal *)realloc(a->data, sizeof(sp_RbVal) * a->cap); } a->data[a->len++] = v; }
static mrb_int sp_PolyArray_length(sp_PolyArray *a) { return a->len; }
static sp_RbVal sp_PolyArray_get(sp_PolyArray *a, mrb_int i) { if (i < 0) i += a->len; return a->data[i]; }

/* Mark the embedded GC reference inside an sp_RbVal (string or obj).
   Used as the scan hook for containers that store polymorphic values. */
static inline void sp_mark_rbval(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) sp_mark_string(v.v.s);
  else if (v.tag == SP_TAG_OBJ) sp_gc_mark(v.v.p);
}

/* StrPolyHash: string keys, sp_RbVal values — for hashes with mixed value types. */
typedef struct{const char**keys;sp_RbVal*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;}sp_StrPolyHash;
static void sp_StrPolyHash_fin(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrPolyHash_scan(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_rbval(h->vals[i]);}}}
static sp_StrPolyHash*sp_StrPolyHash_new(void){sp_StrPolyHash*h=(sp_StrPolyHash*)sp_gc_alloc(sizeof(sp_StrPolyHash),sp_StrPolyHash_fin,sp_StrPolyHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;return h;}
static void sp_StrPolyHash_grow(sp_StrPolyHash*h){mrb_int oc=h->cap;const char**ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_StrPolyHash_get(sp_StrPolyHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return h->vals[idx];idx=(idx+1)&h->mask;}return sp_box_nil();}
static void sp_StrPolyHash_set(sp_StrPolyHash*h,const char*k,sp_RbVal v){if(h->len*2>=h->cap)sp_StrPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrPolyHash_has_key(sp_StrPolyHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(strcmp(h->keys[idx],k)==0)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_StrPolyHash_length(sp_StrPolyHash*h){return h->len;}
static sp_StrArray*sp_StrPolyHash_keys(sp_StrPolyHash*h){sp_StrArray*a=sp_StrArray_new();for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}

/* SymPolyHash: symbol keys, sp_RbVal values — same shape as SymStrHash but with poly values. */
typedef struct{sp_sym*keys;sp_RbVal*vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;}sp_SymPolyHash;
static void sp_SymPolyHash_fin(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_SymPolyHash_scan(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]>=0)sp_mark_rbval(h->vals[i]);}}
static sp_SymPolyHash*sp_SymPolyHash_new(void){sp_SymPolyHash*h=(sp_SymPolyHash*)sp_gc_alloc(sizeof(sp_SymPolyHash),sp_SymPolyHash_fin,sp_SymPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);h->len=0;return h;}
static void sp_SymPolyHash_grow(sp_SymPolyHash*h){mrb_int oc=h->cap;sp_sym*ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]>=0){mrb_int idx=(mrb_int)(((mrb_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_SymPolyHash_get(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return sp_box_nil();}
static void sp_SymPolyHash_set(sp_SymPolyHash*h,sp_sym k,sp_RbVal v){if(h->len*2>=h->cap)sp_SymPolyHash_grow(h);mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_SymPolyHash_has_key(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_SymPolyHash_length(sp_SymPolyHash*h){return h->len;}

#include <setjmp.h>
#define SP_EXC_STACK_MAX 64
static jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];
static const char *sp_exc_msg[SP_EXC_STACK_MAX];
static volatile int sp_exc_top = 0;
static const char *sp_exc_cls[SP_EXC_STACK_MAX];
static volatile const char *sp_last_exc_cls = "";
static void sp_raise_cls(const char *cls, const char *msg) { if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_last_exc_cls = cls; longjmp(sp_exc_stack[sp_exc_top-1], 1); } fprintf(stderr, "unhandled exception: %s\n", msg); exit(1); }
static void sp_raise(const char *msg) { sp_raise_cls("RuntimeError", msg); }
static mrb_bool sp_exc_is_a(const char *cls, const char *target) { return strcmp(cls, target) == 0; }

#define SP_CATCH_STACK_MAX 64
static jmp_buf sp_catch_stack[SP_CATCH_STACK_MAX];
static const char *sp_catch_tag[SP_CATCH_STACK_MAX];
static mrb_int sp_catch_val[SP_CATCH_STACK_MAX];
static volatile int sp_catch_top = 0;
static void sp_throw(const char *tag, mrb_int val) { int i = sp_catch_top - 1; while (i >= 0) { if (strcmp(sp_catch_tag[i], tag) == 0) { sp_catch_val[i] = val; sp_catch_top = i + 1; longjmp(sp_catch_stack[i], 1); } i--; } fprintf(stderr, "uncaught throw: %s\n", tag); exit(1); }

static const char *sp_file_read(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return &("\xff" "")[1]; fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char *buf = sp_str_alloc(sz); if (sz > 0) { size_t r = fread(buf, 1, sz, f); (void)r; } buf[sz] = 0; fclose(f); return buf; }
static void sp_file_write(const char *path, const char *data) { FILE *f = fopen(path, "w"); if (f) { fputs(data, f); fclose(f); } }
static mrb_bool sp_file_exist(const char *path) { FILE *f = fopen(path, "r"); if (f) { fclose(f); return TRUE; } return FALSE; }
static void sp_file_delete(const char *path) { remove(path); }
static const char *sp_backtick(const char *cmd) { FILE *p = popen(cmd, "r"); if (!p) return sp_str_empty; char *buf = sp_str_alloc_raw(4096); size_t n = fread(buf, 1, 4095, p); buf[n] = 0; pclose(p); return buf; }
static const char *sp_file_basename(const char *path) { const char *s = strrchr(path, '/'); if (s) return s + 1; return path; }

typedef mrb_int (*sp_proc_fn_t)(void *, mrb_int);
typedef struct sp_Proc { sp_proc_fn_t fn; void *cap; void (*cap_scan)(void *); } sp_Proc;
static void sp_Proc_scan(void *p) { sp_Proc *pr = (sp_Proc *)p; if (pr->cap && pr->cap_scan) pr->cap_scan(pr->cap); }
static sp_Proc *sp_proc_new(sp_proc_fn_t fn, void *cap, void (*cap_scan)(void *)) { sp_Proc *p = (sp_Proc *)sp_gc_alloc(sizeof(sp_Proc), NULL, sp_Proc_scan); p->fn = fn; p->cap = cap; p->cap_scan = cap_scan; return p; }
static mrb_int sp_proc_call(sp_Proc *p, mrb_int arg) { return (p && p->fn) ? p->fn(p->cap, arg) : 0; }

/* ---- StringIO runtime ---- */
typedef struct { char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed; } sp_StringIO;
static void sio_grow(sp_StringIO *sio, int64_t need) { int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; sio->buf = (char *)realloc(sio->buf, nc + 1); sio->cap = nc; }
static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '\0'; return dl; }
static sp_StringIO *sp_StringIO_new(void) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); s->buf = (char *)calloc(1, 64); s->cap = 63; return s; }
static sp_StringIO *sp_StringIO_new_s(const char *init) { sp_StringIO *s = (sp_StringIO *)calloc(1, sizeof(sp_StringIO)); int64_t l = (int64_t)strlen(init); int64_t c = l < 63 ? 63 : l; s->buf = (char*)malloc(c+1); memcpy(s->buf, init, l); s->buf[l]='\0'; s->len = l; s->cap = c; return s; }
static const char *sp_StringIO_string(sp_StringIO *s) { return s->buf ? s->buf : ""; }
static int64_t sp_StringIO_pos(sp_StringIO *s) { return s->pos; }
static int64_t sp_StringIO_size(sp_StringIO *s) { return s->len; }
static int64_t sp_StringIO_write(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
static int64_t sp_StringIO_puts(sp_StringIO *s, const char *str) { int64_t l = (int64_t)strlen(str); sio_write(s, str, l); if (l == 0 || str[l-1] != '\n') sio_write(s, "\n", 1); return 0; }
static int64_t sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, "\n", 1); return 0; }
static int64_t sp_StringIO_print(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
static int64_t sp_StringIO_putc(sp_StringIO *s, int64_t ch) { char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch; }
static const char *sp_StringIO_read(sp_StringIO *s) { if (s->pos >= s->len) return sp_str_empty; size_t rem = s->len - s->pos; char *r = sp_str_alloc(rem); memcpy(r, s->buf + s->pos, rem); r[rem] = 0; s->pos = s->len; return r; }
static const char *sp_StringIO_read_n(sp_StringIO *s, int64_t n) { if (s->pos >= s->len) return sp_str_empty; int64_t rem = s->len - s->pos; if (n > rem) n = rem; char *r = sp_str_alloc_raw(n+1); memcpy(r, s->buf + s->pos, n); r[n] = '\0'; s->pos += n; return r; }
static const char *sp_StringIO_gets(sp_StringIO *s) { if (s->pos >= s->len) return NULL; const char *st = s->buf + s->pos; const char *nl = memchr(st, '\n', s->len - s->pos); int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos; char *r = sp_str_alloc_raw(ll+1); memcpy(r, st, ll); r[ll] = '\0'; s->pos += ll; s->lineno++; return r; }
static const char *sp_StringIO_getc(sp_StringIO *s) { if (s->pos >= s->len) return NULL; char *gc = sp_str_alloc_raw(2); gc[0] = s->buf[s->pos++]; gc[1] = '\0'; return gc; }
static int64_t sp_StringIO_getbyte(sp_StringIO *s) { if (s->pos >= s->len) return -1; return (int64_t)(unsigned char)s->buf[s->pos++]; }
static int64_t sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }
static int64_t sp_StringIO_seek(sp_StringIO *s, int64_t off) { if (off < 0) off = 0; s->pos = off; return 0; }
static int64_t sp_StringIO_tell(sp_StringIO *s) { return s->pos; }
static mrb_bool sp_StringIO_eof_p(sp_StringIO *s) { return s->pos >= s->len; }
static int64_t sp_StringIO_truncate(sp_StringIO *s, int64_t l) { if (l < 0) l = 0; if (l < s->len) { s->len = l; s->buf[l] = '\0'; } return 0; }
static int64_t sp_StringIO_close(sp_StringIO *s) { s->closed = 1; return 0; }
static mrb_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed; }
static sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }
static mrb_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }
static mrb_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }

/* ---- Lambda/closure runtime (sp_Val) ---- */
typedef struct sp_Val sp_Val;
typedef sp_Val *(*sp_fn_t)(sp_Val *self, sp_Val *arg);
struct sp_Val { enum { SP_PROC2, SP_INT2, SP_BOOL2, SP_NIL2 } tag; union { struct { sp_fn_t fn; int ncaptures; } proc; mrb_int ival; mrb_bool bval; } u; sp_Val *captures[]; };
#ifdef _WIN32
#define SP_ARENA_SIZE ((size_t)256ULL * 1024 * 1024)
#else
#define SP_ARENA_SIZE ((size_t)16ULL * 1024 * 1024 * 1024)
#endif
static char *sp_arena = NULL; static size_t sp_arena_pos = 0;
static void *sp_lam_alloc(size_t sz) { sz = (sz + 7) & ~(size_t)7; if (!sp_arena) { sp_arena = (char *)mmap(NULL, SP_ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0); if (sp_arena == MAP_FAILED) { perror("mmap"); exit(1); } sp_arena_pos = 0; } if (sp_arena_pos + sz > SP_ARENA_SIZE) { fprintf(stderr, "arena exhausted\n"); exit(1); } void *p = sp_arena + sp_arena_pos; sp_arena_pos += sz; return p; }
static sp_Val *sp_lam_proc(sp_fn_t fn, int ncap) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val) + sizeof(sp_Val *) * ncap); v->tag = SP_PROC2; v->u.proc.fn = fn; v->u.proc.ncaptures = ncap; return v; }
static sp_Val *sp_lam_int(mrb_int n) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_INT2; v->u.ival = n; return v; }
static sp_Val *sp_lam_bool(mrb_bool b) { sp_Val *v = (sp_Val *)sp_lam_alloc(sizeof(sp_Val)); v->tag = SP_BOOL2; v->u.bval = b; return v; }
static sp_Val sp_lam_nil_val = { .tag = SP_NIL2 };
static sp_Val *sp_lam_call(sp_Val *f, sp_Val *arg) { return f->u.proc.fn(f, arg); }
static mrb_int sp_lam_to_int(sp_Val *v) { return v->u.ival; }

/* ---- Fiber runtime (ucontext) ---- */
#ifdef _WIN32
#define SP_FIBER_STACK_SIZE (64*1024)
typedef struct sp_Fiber{LPVOID ctx;LPVOID caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;struct sp_Fiber*caller;}sp_Fiber;
static sp_Fiber sp_fiber_root;
static sp_Fiber*sp_fiber_current=&sp_fiber_root;
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->ctx)DeleteFiber(f->ctx);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);}
static sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->ctx=NULL;f->caller_ctx=NULL;f->stack=NULL;f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->caller=NULL;return f;}
static VOID CALLBACK sp_fiber_trampoline(LPVOID param){sp_Fiber*f=(sp_Fiber*)param;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;SwitchToFiber(sp_fiber_root.ctx);}else{SwitchToFiber(f->caller->ctx);}}
static void sp_fiber_ensure_root(void){if(!sp_fiber_root.ctx){sp_fiber_root.ctx=ConvertThreadToFiber(NULL);if(!sp_fiber_root.ctx)sp_fiber_root.ctx=GetCurrentFiber();}}
static sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}sp_fiber_ensure_root();f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;f->caller=prev;sp_fiber_current=f;if(f->state==0){f->state=1;f->ctx=CreateFiber(SP_FIBER_STACK_SIZE,sp_fiber_trampoline,f);if(!f->ctx)sp_raise("failed to create fiber");}else{f->state=1;}SwitchToFiber(f->ctx);sp_fiber_current=prev;return f->yielded_value;}
static sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;SwitchToFiber(f->caller->ctx);return f->resumed_value;}
static mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
static sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){sp_fiber_ensure_root();f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;f->caller=prev;f->ctx=CreateFiber(SP_FIBER_STACK_SIZE,sp_fiber_trampoline,f);if(!f->ctx)sp_raise("failed to create fiber");}else{f->state=1;}SwitchToFiber(f->ctx);sp_fiber_current=prev;return prev->resumed_value;}
#else
#include <ucontext.h>
#include <sys/mman.h>
#define SP_FIBER_STACK_SIZE (64*1024)
typedef struct sp_Fiber{ucontext_t ctx;ucontext_t caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;}sp_Fiber;
static sp_Fiber sp_fiber_root;
static sp_Fiber*sp_fiber_current=&sp_fiber_root;
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,SP_FIBER_STACK_SIZE);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);}
static sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->stack=(char*)mmap(NULL,SP_FIBER_STACK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;return f;}
static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;setcontext(&sp_fiber_root.ctx);}else{swapcontext(&f->ctx,&f->caller_ctx);}}
static sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_current=f;if(f->state==0){f->state=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&f->caller_ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&f->caller_ctx,&f->ctx);}else{f->state=1;swapcontext(&f->caller_ctx,&f->ctx);}sp_fiber_current=prev;return f->yielded_value;}
static sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;swapcontext(&f->ctx,&f->caller_ctx);return f->resumed_value;}
static mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
static sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&prev->ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&prev->ctx,&f->ctx);}else{f->state=1;swapcontext(&prev->ctx,&f->ctx);}sp_fiber_current=prev;return prev->resumed_value;}
#endif

/* Bigint (linked from sp_bigint.o) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);


/* System/backtick support */
static int sp_last_status = 0;

/* Bigint (linked from libspinel_rt.a) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

#endif /* SP_RUNTIME_H */
