#ifndef SP_ARRAY_H
#define SP_ARRAY_H
/* sp_array.h -- typed array hot core + cold-op surface.
 *
 * The struct layouts (sp_IntArray, ...) live in sp_types.h. The hot
 * accessors (new / push / pop / shift / get / set / length / empty) stay
 * inline here so every generated TU compiles them identically --
 * relocating them out of spinel_rt.h into this shared header is a pure
 * textual move with no codegen change. The cold ops (sort / slice / dup /
 * set algebra / join / ...) are compiled once into libspinel_rt.a
 * (lib/sp_array.c); this header only declares them.
 *
 * sp_sprintf / sp_raise_cls / sp_raise_frozen_array are provided by the
 * generated TU and resolved at the final link, the same way lib/sp_core.c
 * calls them -- so lib/sp_array.c can use them without a runtime include.
 */
#include <stdint.h>
#include <math.h>      /* isnan / isinf / signbit / NAN / fabs for sp_float_sum_step */
#include "sp_gc.h"      /* sp_gc_hdr, sp_gc_bytes, SP_GC_ROOT, sp_oom_die */
#include "sp_alloc.h"   /* sp_gc_alloc, sp_str_alloc, sp_raise_cls, sp_raise_frozen_array */

const char *sp_sprintf(const char *fmt, ...);  /* defined in the generated TU */

/* ============================ nil bitmaps ============================ */
/* The nil bitmap of an Integer or Float array (sp_types.h: `nilbits`, one bit
   per physical slot, NULL until the first nil, sized to `cap` once it
   exists, clear outside the live window). The bitmap's bytes are counted to
   the owning array's GC header like its data. Every path that touches a
   bitmap is cold: the arrays of a program that never stores a nil into a
   typed array keep NULL and pay one pointer test where a read must know. */
static SP_NOINLINE SP_COLD uint64_t *sp_nilbits_new(void *owner, sp_int cap) {
  size_t nb = sp_nilbits_words(cap) * sizeof(uint64_t);
  uint64_t *b = (uint64_t *)sp_pl_zalloc(nb ? nb : sizeof(uint64_t));
  if (!b) sp_oom_die();
  { sp_gc_hdr *h = (sp_gc_hdr *)((char *)owner - sizeof(sp_gc_hdr)); h->size += nb; sp_gc_bytes_add(nb); }
  return b;
}
/* `cap` grew (or shrank) from oldcap to newcap: the bitmap follows, new words clear */
static SP_NOINLINE SP_COLD uint64_t *sp_nilbits_resize(void *owner, uint64_t *b, sp_int oldcap, sp_int newcap) {
  size_t ow = sp_nilbits_words(oldcap), nw = sp_nilbits_words(newcap);
  if (ow == nw) return b;
  { sp_gc_hdr *h = (sp_gc_hdr *)((char *)owner - sizeof(sp_gc_hdr)); h->size -= ow * sizeof(uint64_t); sp_gc_bytes_sub(ow * sizeof(uint64_t)); }
  b = (uint64_t *)sp_pl_realloc(b, (nw ? nw : 1) * sizeof(uint64_t));
  if (!b) sp_oom_die();
  if (nw > ow) memset(b + ow, 0, (nw - ow) * sizeof(uint64_t));
  { sp_gc_hdr *h = (sp_gc_hdr *)((char *)owner - sizeof(sp_gc_hdr)); h->size += nw * sizeof(uint64_t); sp_gc_bytes_add(nw * sizeof(uint64_t)); }
  return b;
}
static SP_NOINLINE SP_COLD void sp_nilbits_free(void *owner, uint64_t *b, sp_int cap) {
  size_t nb = sp_nilbits_words(cap) * sizeof(uint64_t);
  if (!b) return;
  if (owner) { sp_gc_hdr *h = (sp_gc_hdr *)((char *)owner - sizeof(sp_gc_hdr)); h->size -= nb; sp_gc_bytes_sub(nb); }
  sp_pl_free(b);
}
/* bits [from, to) */
static inline void sp_nilbits_clear_range(uint64_t *b, sp_int from, sp_int to) {
  for (sp_int i = from; i < to; i++) sp_nilbit_clr(b, i);
}
static inline void sp_nilbits_set_range(uint64_t *b, sp_int from, sp_int to) {
  for (sp_int i = from; i < to; i++) sp_nilbit_set(b, i);
}
static inline int sp_nilbits_any(const uint64_t *b, sp_int from, sp_int to) {
  for (sp_int i = from; i < to; i++) if (sp_nilbit_get(b, i)) return 1;
  return 0;
}
static inline sp_int sp_nilbits_count(const uint64_t *b, sp_int from, sp_int to) {
  sp_int n = 0;
  for (sp_int i = from; i < to; i++) n += sp_nilbit_get(b, i);
  return n;
}
/* a block of elements moved from `from` to `to` (a memmove of the data): bit
   to+i takes bit from+i for i in [0, n); the bits of the vacated slots are
   clear, and bits outside both spans keep their value */
static SP_NOINLINE SP_COLD void sp_nilbits_move(uint64_t *b, sp_int cap, sp_int from, sp_int to, sp_int n) {
  if (from == to || n <= 0) return;
  size_t nw = sp_nilbits_words(cap);
  uint64_t *t = (uint64_t *)sp_pl_zalloc((nw ? nw : 1) * sizeof(uint64_t));
  if (!t) sp_oom_die();
  for (sp_int i = 0; i < n; i++) if (sp_nilbit_get(b, from + i)) sp_nilbit_set(t, to + i);
  sp_nilbits_clear_range(b, from, from + n);
  sp_nilbits_clear_range(b, to, to + n);
  for (size_t w = 0; w < nw; w++) b[w] |= t[w];
  sp_pl_free(t);
}

/* nil_lo: the lowest physical slot that can hold a nil, read only while the
   array has a bitmap (a conservative bound: never above the lowest set bit).
   A loop's cached read below it needs no bitmap test (SP_INT_NILFREE_LEN).
   The bitmap's birth sets it to none, every bit set lowers it, a move of
   the bits (a memmove of the elements) drops it to 0; clearing a bit
   leaves it. */
#define SP_NIL_LO_NONE UINT32_MAX
#define SP_NILBITS_NEW_A(a) ((a)->nil_lo = SP_NIL_LO_NONE, (a)->nilbits = sp_nilbits_new((a), (a)->cap))
#define SP_NILBIT_SET_A(a, p) ({ sp_int _nb_p = (p); \
    if ((uint64_t)_nb_p < (uint64_t)(a)->nil_lo) (a)->nil_lo = (uint32_t)_nb_p; \
    sp_nilbit_set((a)->nilbits, _nb_p); })
#define SP_NILBITS_SET_RANGE_A(a, lo, hi) ({ sp_int _nb_lo = (lo), _nb_hi = (hi); \
    if (_nb_lo < _nb_hi && (uint64_t)_nb_lo < (uint64_t)(a)->nil_lo) (a)->nil_lo = (uint32_t)_nb_lo; \
    sp_nilbits_set_range((a)->nilbits, _nb_lo, _nb_hi); })
#define SP_NILBITS_MOVE_A(a, ...) ({ sp_nilbits_move((a)->nilbits, __VA_ARGS__); (a)->nil_lo = 0; })
/* the length of the leading span a loop may read with no bitmap test:
   all of it without a bitmap, else up to nil_lo (in element positions) */
#define SP_INT_NILFREE_LEN(a) (!(a)->nilbits ? (a)->len : \
    ((sp_int)(a)->nil_lo <= (a)->start ? 0 : ((sp_int)(a)->nil_lo - (a)->start < (a)->len ? (sp_int)(a)->nil_lo - (a)->start : (a)->len)))
#define SP_FLOAT_NILFREE_LEN(a) (!(a)->nilbits ? (a)->len : \
    ((sp_int)(a)->nil_lo < (a)->len ? (sp_int)(a)->nil_lo : (a)->len))

/* ============================ sp_IntArray ============================ */
/* `frozen` rides in the struct (not the GC header) so the hot push /
   []= paths read it from the same cache line as len/cap -- no extra
   cache miss vs. the GC-header bit. calloc in sp_gc_alloc zero-inits
   it, so constructors need no change. Issue #918. */
static void sp_IntArray_fin(void*p){sp_IntArray*a=(sp_IntArray*)p;sp_pl_free(a->data);sp_pl_free(a->nilbits);}
static sp_IntArray*sp_IntArray_new(void){sp_IntArray*a=(sp_IntArray*)sp_gc_alloc(sizeof(sp_IntArray),sp_IntArray_fin,NULL);a->cap=16;a->data=(sp_int*)sp_pl_alloc(sizeof(sp_int)*a->cap);if(!a->data)sp_oom_die();a->start=0;a->len=0;a->nilbits=NULL;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_int)*a->cap;sp_gc_bytes_add(sizeof(sp_int)*a->cap);}return a;}
/* An Array set up inside a bigger object that starts with it -- an Array
   subclass instance, whose struct embeds its Array (#7449) -- as _new sets a
   fresh one up; the payload is counted to the enclosing object's header. */
static void sp_IntArray_init_embedded(sp_IntArray*a){a->cap=16;a->data=(sp_int*)sp_pl_alloc(sizeof(sp_int)*a->cap);if(!a->data)sp_oom_die();a->start=0;a->len=0;a->nilbits=NULL;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_int)*a->cap;sp_gc_bytes_add(sizeof(sp_int)*a->cap);}}
/* The one place `cap` changes: the data buffer is reallocated and
   re-counted, and the nil bitmap (when there is one) follows it. */
static SP_NOINLINE void sp_IntArray_set_cap(sp_IntArray*a,sp_int nc){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_int oc=a->cap;if((uintmax_t)nc>SIZE_MAX/sizeof(sp_int))sp_oom_die();sp_gc_bytes_sub(sizeof(sp_int)*oc);h->size-=sizeof(sp_int)*oc;void*nd=sp_pl_realloc(a->data,sizeof(sp_int)*nc);if(!nd)sp_oom_die();a->data=(sp_int*)nd;a->cap=nc;h->size+=sizeof(sp_int)*nc;sp_gc_bytes_add(sizeof(sp_int)*nc);if(SP_UNLIKELY(a->nilbits))a->nilbits=sp_nilbits_resize(a,a->nilbits,oc,nc);}
static SP_NOINLINE void sp_IntArray_push_grow(sp_IntArray*a){if(a->start>0){memmove(a->data,a->data+a->start,sizeof(sp_int)*a->len);if(SP_UNLIKELY(a->nilbits))SP_NILBITS_MOVE_A(a, a->cap, a->start, 0, a->len);a->start=0;if(a->len<a->cap)return;}sp_IntArray_set_cap(a,a->cap*2+1);}
static inline void sp_IntArray_push(sp_IntArray*a,sp_int v){if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}if(a->start+a->len>=a->cap)sp_IntArray_push_grow(a);a->data[a->start+a->len]=v;a->len++;}
/* Array.new(n, v): the buffer is allocated at its final size and filled in
   place, where n pushes grew it from 16 by doubling -- a realloc and a copy
   at every step, and a frozen and a capacity test per element. */
static sp_IntArray*sp_IntArray_new_fill(sp_int n,sp_int v){sp_IntArray*a=(sp_IntArray*)sp_gc_alloc(sizeof(sp_IntArray),sp_IntArray_fin,NULL);if((uintmax_t)n>SIZE_MAX/sizeof(sp_int))sp_oom_die();a->cap=n>16?n:16;a->data=(sp_int*)sp_pl_alloc(sizeof(sp_int)*a->cap);if(!a->data)sp_oom_die();a->start=0;a->nilbits=NULL;for(sp_int i=0;i<n;i++)a->data[i]=v;a->len=n;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_int)*a->cap;sp_gc_bytes_add(sizeof(sp_int)*a->cap);}return a;}
/* Array.new(n) with no default: n nils, so the bitmap exists from the start
   with every live bit set (and the data zeroed, so a bit cleared by a later
   store leaves a real 0 behind only where the store wrote one). */
static SP_COLD sp_IntArray*sp_IntArray_new_nil(sp_int n){sp_IntArray*a=sp_IntArray_new_fill(n,0);if(n>0){SP_NILBITS_NEW_A(a);SP_NILBITS_SET_RANGE_A(a, 0, n);}return a;}
/* Reads and removals of elements that can be nil answer sp_oint. The bit of
   an element leaving the live window is cleared, so the window's outside
   stays clear (sp_types.h) and a push never writes the bitmap. */
static SP_NOINLINE SP_COLD sp_oint sp_IntArray_take_cold(sp_IntArray*a,sp_int pi,sp_int v){if(sp_nilbit_get(a->nilbits,pi)){sp_nilbit_clr(a->nilbits,pi);return sp_oint_nil();}return sp_oint_of(v);}
/* Issue #826/#832: empty pop/shift answer nil to match MRI; without the
   guard, `--a->len` wraps to -1 and reads past the buffer start. */
static inline sp_oint sp_IntArray_pop_o(sp_IntArray*a){if(!a||a->len<=0)return sp_oint_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return sp_oint_nil();}sp_int pi=a->start+--a->len;if(SP_UNLIKELY(a->nilbits))return sp_IntArray_take_cold(a,pi,a->data[pi]);return sp_oint_of(a->data[pi]);}
static inline sp_oint sp_IntArray_shift_o(sp_IntArray*a){if(!a||a->len<=0)return sp_oint_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return sp_oint_nil();}sp_int pi=a->start;a->start++;a->len--;if(SP_UNLIKELY(a->nilbits))return sp_IntArray_take_cold(a,pi,a->data[pi]);return sp_oint_of(a->data[pi]);}
static inline sp_int sp_IntArray_length(sp_IntArray*a){return a->len;}
static inline sp_bool sp_IntArray_empty(sp_IntArray*a){return a->len==0;}
/* `a[i]` where the read is proven in range and the element proven not nil:
   the value, with no bitmap test. (Out of range, or on a nil element, it
   answers the slot's 0 -- a read the emitter never makes.) */
static inline sp_int sp_IntArray_get(sp_IntArray*a,sp_int i){if(!a)return 0;if((unsigned long long)i<(unsigned long long)a->len)return a->data[a->start+i];if(i<0)i+=a->len;if(i<0||i>=a->len)return 0;return a->data[a->start+i];}
/* is element i (in range) nil? */
static inline sp_bool sp_IntArray_elem_nil(sp_IntArray*a,sp_int i){return SP_UNLIKELY(a->nilbits!=NULL)&&sp_nilbit_get(a->nilbits,a->start+i);}
/* `a[i]` that can miss: out of range is nil, a set bit is nil, else the value */
/* The element read with its nil. The fast path -- an in-range index of an
   array that never held a nil -- is all that inlines: a compare, the load,
   and the bitmap test after it (the load first, so the header's data/start
   pair stays one load). A negative index, one past the end, a bitmap, a nil
   receiver take the cold call; gcc kept the whole read out of line at sites
   of a large function (optcarrot's CPU#run) once it grew those arms. */
static SP_NOINLINE SP_COLD sp_oint sp_IntArray_oget_slow(sp_IntArray*a,sp_int i){if(!a)return sp_oint_nil();if(!((unsigned long long)i<(unsigned long long)a->len)){if(i<0)i+=a->len;if(i<0||i>=a->len)return sp_oint_nil();}sp_int pi=a->start+i;sp_int v=a->data[pi];if(a->nilbits&&sp_nilbit_get(a->nilbits,pi))return sp_oint_nil();return sp_oint_of(v);}
static SP_INLINE sp_oint sp_IntArray_oget(sp_IntArray*a,sp_int i){if(SP_LIKELY(a&&(unsigned long long)i<(unsigned long long)a->len)){sp_int v=a->data[a->start+i];if(SP_LIKELY(!a->nilbits))return sp_oint_of(v);}return sp_IntArray_oget_slow(a,i);}
/* Issue #769: a very-negative i leaves i negative after the `i += a->len`
   adjustment. CRuby raises IndexError; spinel no-ops as the safest
   fallback (raising from a typed-array set would need setjmp plumbing
   throughout the call chain). */
static void sp_IntArray_set_slow(sp_IntArray*a,sp_int i,sp_int v){if(i<0)return;while(a->start+i>=a->cap)sp_IntArray_set_cap(a,a->cap*2+1);if(i>a->len){/* gap slots read as nil */if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBITS_SET_RANGE_A(a, a->start+a->len, a->start+i);while(i>a->len){a->data[a->start+a->len]=0;a->len++;}}if(i==a->len)a->len++;a->data[a->start+i]=v;if(SP_UNLIKELY(a->nilbits))sp_nilbit_clr(a->nilbits,a->start+i);}
/* Issue #839: an extreme negative index (still negative after `i += len`)
   raises IndexError per MRI. */
static SP_NOINLINE SP_COLD void sp_IntArray_set_cold(sp_IntArray*a,sp_int i,sp_int v){if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));if(i<a->len){a->data[a->start+i]=v;if(SP_UNLIKELY(a->nilbits))sp_nilbit_clr(a->nilbits,a->start+i);return;}sp_IntArray_set_slow(a,i,v);}
/* the bitmap write of an in-range store, off the store's path */
static SP_NOINLINE SP_COLD void sp_IntArray_clr_nil(sp_IntArray*a,sp_int pi){sp_nilbit_clr(a->nilbits,pi);}
static inline void sp_IntArray_set(sp_IntArray*a,sp_int i,sp_int v){if(SP_LIKELY(a&&!a->frozen&&i>=0&&i<a->len)){a->data[a->start+i]=v;if(SP_UNLIKELY(a->nilbits))sp_IntArray_clr_nil(a,a->start+i);return;}sp_IntArray_set_cold(a,i,v);}
/* `a[i] = nil`: the slot's value is 0 and its bit set (the index rules of
   sp_IntArray_set: a negative index from the end, IndexError below -len, a
   gap of nils past the end) */
static SP_NOINLINE SP_COLD void sp_IntArray_set_nil(sp_IntArray*a,sp_int i){if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));if(i>=a->len)sp_IntArray_set_slow(a,i,0);else a->data[a->start+i]=0;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->start+i);}
static inline void sp_IntArray_oset(sp_IntArray*a,sp_int i,sp_oint o){if(SP_UNLIKELY(o.nil))sp_IntArray_set_nil(a,i);else sp_IntArray_set(a,i,o.v);}
static SP_NOINLINE SP_COLD void sp_IntArray_push_nil(sp_IntArray*a){sp_IntArray_push(a,0);if(a->frozen)return;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->start+a->len-1);}
static inline void sp_IntArray_push_o(sp_IntArray*a,sp_oint o){if(SP_UNLIKELY(o.nil))sp_IntArray_push_nil(a);else sp_IntArray_push(a,o.v);}
/* The stores of a value that may be nil, as the emitter spelled them before
   the nil moved out of band: macros, not inline functions, since a large
   generated unit sits at gcc's inlining budget and a wrapper the inliner
   had to spend it on pushed the hot accessors of an unrelated loop out of
   line (optcarrot's PPU loop). */
#define sp_IntArray_push_nilable(a, o) sp_IntArray_push_o((a), (o))
#define sp_IntArray_set_nilable(a, i, o) sp_IntArray_oset((a), (i), (o))
/* An array built from another carries its bitmap in the building op
   itself (dup, slice, sort, concat); nothing is left for the caller to do. */
#define sp_IntArray_nil_from(d, s) ((void)0)
#define sp_IntArray_may_nil(a) ({ const sp_IntArray *_mn_a = (a); (sp_bool)(_mn_a && SP_MAY_NIL(_mn_a)); })
/* does the array hold a nil right now? (the bitmap scanned over the live window) */
static inline sp_bool sp_IntArray_has_nil(sp_IntArray*a){return a&&a->nilbits&&sp_nilbits_any(a->nilbits,a->start,a->start+a->len);}
static inline sp_int sp_IntArray_nil_count(sp_IntArray*a){return (a&&a->nilbits)?sp_nilbits_count(a->nilbits,a->start,a->start+a->len):0;}
/* the bitmap dropped once no nil is left (compact!, delete(nil), clear, replace) */
static SP_COLD void sp_IntArray_drop_nilbits(sp_IntArray*a){if(a&&a->nilbits){sp_nilbits_free(a,a->nilbits,a->cap);a->nilbits=NULL;}}
/* element i of `a` boxed: nil where the bitmap says so */
/* an index past the end (zip's padding, a gap) is nil, as the read is */
static inline sp_RbVal sp_IntArray_box_elem(sp_IntArray*a,sp_int i){if(!a||i<0||i>=a->len||sp_IntArray_elem_nil(a,i))return sp_box_nil();return sp_box_int(a->data[a->start+i]);}

/* ---- sp_IntArray cold ops (compiled in lib/sp_array.c) ---- */
sp_IntArray *sp_IntArray_from_range(sp_int s, sp_int e);
sp_IntArray *sp_IntArray_from_range_step(sp_int beg, sp_int end, sp_int step, sp_int excl);
sp_IntArray *sp_IntArray_dup(sp_IntArray *a);
sp_IntArray *sp_IntArray_slice(sp_IntArray *a, sp_int start, sp_int len);
sp_IntArray *sp_IntArray_slice_range(sp_IntArray *a, sp_int start, sp_int end_, sp_int excl);
void sp_IntArray_replace(sp_IntArray *dst, sp_IntArray *src);
/* a[start, len] = plain values (no nil among them) */
void sp_IntArray_splice(sp_IntArray *a, sp_int start, sp_int len, const sp_int *src, sp_int srcn);
void sp_FloatArray_splice(sp_FloatArray *a, sp_int start, sp_int len, const sp_float *src, sp_int srcn);
/* a[start, len] = a whole typed array, whose nil elements come along */
void sp_IntArray_splice_o(sp_IntArray *a, sp_int start, sp_int len, sp_IntArray *src);
void sp_FloatArray_splice_o(sp_FloatArray *a, sp_int start, sp_int len, sp_FloatArray *src);
void sp_StrArray_splice(sp_StrArray *a, sp_int start, sp_int len, const char *const *src, sp_int srcn);
void sp_PolyArray_splice(sp_PolyArray *a, sp_int start, sp_int len, sp_RbVal src);
void sp_IntArray_reverse_bang(sp_IntArray *a);
void sp_IntArray_rotate_bang(sp_IntArray *a, sp_int n);
sp_IntArray *sp_IntArray_sort(sp_IntArray *a);
void sp_IntArray_sort_bang(sp_IntArray *a);
void sp_IntArray_uniq_bang(sp_IntArray *a);
void sp_IntArray_shuffle_bang(sp_IntArray *a);
sp_IntArray *sp_IntArray_shuffle(sp_IntArray *a);
sp_oint sp_IntArray_sample_o(sp_IntArray *a);
sp_oint sp_IntArray_min_o(sp_IntArray *a);
sp_oint sp_IntArray_max_o(sp_IntArray *a);
const char *sp_StrArray_min(sp_StrArray *a);
const char *sp_StrArray_max(sp_StrArray *a);
sp_int sp_IntArray_sum(sp_IntArray *a, sp_int init);
/* A vectorizable reduction for nonnegative, bounded elements. The OR is
   an upper bound for every term, so the bound covers every ordered prefix.
   Otherwise resume with checked additions, stopping before the first overflow. */
static inline sp_int sp_IntArray_sum_prefix(sp_IntArray *a, sp_int seed, sp_int *sum) {
  uintptr_t total = 0, bound = 0;
  for (sp_int i = 0; i < a->len; i++) {
    uintptr_t v = (uintptr_t)a->data[a->start + i];
    total += v;
    bound |= v;
  }
  uint64_t limit;
  if (bound <= INTPTR_MAX &&
      !sp_ckd_mul_u64(bound, (uint64_t)a->len, &limit) &&
      limit <= (uintptr_t)INTPTR_MAX - (uintptr_t)seed) {
    *sum = (sp_int)((uintptr_t)seed + total);
    return a->len;
  }
  for (sp_int i = 0; i < a->len; i++) {
    sp_int next;
    if (sp_ckd_add_iptr(seed, a->data[a->start + i], &next)) {
      *sum = seed;
      return i;
    }
    seed = next;
  }
  *sum = seed;
  return a->len;
}
sp_bool sp_IntArray_include(sp_IntArray *a, sp_int v);
sp_int sp_IntArray_index(sp_IntArray *a, sp_int v);
sp_int sp_IntArray_rindex(sp_IntArray *a, sp_int v);
sp_oint sp_IntArray_delete_at_o(sp_IntArray *a, sp_int i);
sp_oint sp_IntArray_delete_o(sp_IntArray *a, sp_int v);
/* delete(nil): every nil element removed, as CRuby; answers whether one was */
sp_bool sp_IntArray_delete_nil(sp_IntArray *a);
void sp_IntArray_insert(sp_IntArray *a, sp_int i, sp_int v);
void sp_IntArray_insert_nil(sp_IntArray *a, sp_int i);
sp_IntArray *sp_IntArray_uniq(sp_IntArray *a);
sp_IntArray *sp_IntArray_intersect(sp_IntArray *a, sp_IntArray *b);
sp_bool sp_IntArray_intersect_p(sp_IntArray *a, sp_IntArray *b);
sp_IntArray *sp_IntArray_union(sp_IntArray *a, sp_IntArray *b);
sp_IntArray *sp_IntArray_difference(sp_IntArray *a, sp_IntArray *b);
void sp_IntArray_unshift(sp_IntArray *a, sp_int v);
void sp_IntArray_unshift_nil(sp_IntArray *a);
const char *sp_IntArray_join(sp_IntArray *a, const char *sep);
sp_bool sp_IntArray_eq(sp_IntArray *a, sp_IntArray *b);
sp_oint sp_IntArray_cmp_o(sp_IntArray *a, sp_IntArray *b);   /* <=>: nil once a nil meets a number */

/* =========================== sp_FloatArray =========================== */
static void sp_FloatArray_fin(void*p){sp_FloatArray*a=(sp_FloatArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes_sub(sizeof(sp_float)*a->cap);h->size-=sizeof(sp_float)*a->cap;sp_pl_free(a->data);sp_pl_free(a->nilbits);}
static sp_FloatArray*sp_FloatArray_new(void){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);a->cap=16;a->data=(sp_float*)sp_pl_alloc(sizeof(sp_float)*a->cap);if(!a->data)sp_oom_die();a->len=0;a->nilbits=NULL;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_float)*a->cap;sp_gc_bytes_add(sizeof(sp_float)*a->cap);}return a;}
static void sp_FloatArray_init_embedded(sp_FloatArray*a){a->cap=16;a->data=(sp_float*)sp_pl_alloc(sizeof(sp_float)*a->cap);if(!a->data)sp_oom_die();a->len=0;a->nilbits=NULL;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_float)*a->cap;sp_gc_bytes_add(sizeof(sp_float)*a->cap);}}  /* see sp_IntArray_init_embedded */
/* see sp_IntArray_set_cap */
static SP_NOINLINE void sp_FloatArray_set_cap(sp_FloatArray*a,sp_int nc){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_int oc=a->cap;if((uintmax_t)nc>SIZE_MAX/sizeof(sp_float))sp_oom_die();sp_gc_bytes_sub(sizeof(sp_float)*oc);h->size-=sizeof(sp_float)*oc;void*nd=sp_pl_realloc(a->data,sizeof(sp_float)*nc);if(!nd)sp_oom_die();a->data=(sp_float*)nd;a->cap=nc;h->size+=sizeof(sp_float)*nc;sp_gc_bytes_add(sizeof(sp_float)*nc);if(SP_UNLIKELY(a->nilbits))a->nilbits=sp_nilbits_resize(a,a->nilbits,oc,nc);}
static inline void sp_FloatArray_push(sp_FloatArray*a,sp_float v){if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}if(a->len>=a->cap)sp_FloatArray_set_cap(a,a->cap*2+1);a->data[a->len++]=v;}
/* Array.new(n, v): see sp_IntArray_new_fill */
static sp_FloatArray*sp_FloatArray_new_fill(sp_int n,sp_float v){sp_FloatArray*a=(sp_FloatArray*)sp_gc_alloc(sizeof(sp_FloatArray),sp_FloatArray_fin,NULL);if((uintmax_t)n>SIZE_MAX/sizeof(sp_float))sp_oom_die();a->cap=n>16?n:16;a->data=(sp_float*)sp_pl_alloc(sizeof(sp_float)*a->cap);if(!a->data)sp_oom_die();a->nilbits=NULL;for(sp_int i=0;i<n;i++)a->data[i]=v;a->len=n;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(sp_float)*a->cap;sp_gc_bytes_add(sizeof(sp_float)*a->cap);}return a;}
static SP_COLD sp_FloatArray*sp_FloatArray_new_nil(sp_int n){sp_FloatArray*a=sp_FloatArray_new_fill(n,0.0);if(n>0){SP_NILBITS_NEW_A(a);SP_NILBITS_SET_RANGE_A(a, 0, n);}return a;}
static SP_NOINLINE SP_COLD sp_ofloat sp_FloatArray_take_cold(sp_FloatArray*a,sp_int i,sp_float v){if(sp_nilbit_get(a->nilbits,i)){sp_nilbit_clr(a->nilbits,i);return sp_ofloat_nil();}return sp_ofloat_of(v);}
/* CRuby answers nil for pop/shift on an empty array (#4288). */
static inline sp_ofloat sp_FloatArray_pop_o(sp_FloatArray*a){if(!a||a->len<=0)return sp_ofloat_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return sp_ofloat_nil();}sp_int i=--a->len;if(SP_UNLIKELY(a->nilbits))return sp_FloatArray_take_cold(a,i,a->data[i]);return sp_ofloat_of(a->data[i]);}
/* FloatArray is 0-based (no `start` offset, unlike IntArray): a shift moves
   the elements, and their bits with them. */
static SP_NOINLINE SP_COLD sp_ofloat sp_FloatArray_shift_cold(sp_FloatArray*a){sp_ofloat r=sp_nilbit_get(a->nilbits,0)?sp_ofloat_nil():sp_ofloat_of(a->data[0]);for(sp_int i=0;i+1<a->len;i++)a->data[i]=a->data[i+1];SP_NILBITS_MOVE_A(a, a->cap, 1, 0, a->len-1);a->len--;return r;}
static inline sp_ofloat sp_FloatArray_shift_o(sp_FloatArray*a){if(!a||a->len==0)return sp_ofloat_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return sp_ofloat_nil();}if(SP_UNLIKELY(a->nilbits))return sp_FloatArray_shift_cold(a);sp_float v=a->data[0];for(sp_int i=0;i+1<a->len;i++)a->data[i]=a->data[i+1];a->len--;return sp_ofloat_of(v);}
static SP_NOINLINE SP_COLD sp_ofloat sp_FloatArray_delete_at_cold(sp_FloatArray*a,sp_int i){sp_ofloat r=sp_nilbit_get(a->nilbits,i)?sp_ofloat_nil():sp_ofloat_of(a->data[i]);for(sp_int j=i;j+1<a->len;j++)a->data[j]=a->data[j+1];if(i+1<a->len)SP_NILBITS_MOVE_A(a, a->cap, i+1, i, a->len-i-1);else sp_nilbit_clr(a->nilbits,i);a->len--;return r;}
/* delete_at: nil out of range, as CRuby */
static inline sp_ofloat sp_FloatArray_delete_at_o(sp_FloatArray*a,sp_int i){if(!a)return sp_ofloat_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return sp_ofloat_nil();}if(i<0)i+=a->len;if(i<0||i>=a->len)return sp_ofloat_nil();if(SP_UNLIKELY(a->nilbits))return sp_FloatArray_delete_at_cold(a,i);sp_float v=a->data[i];for(sp_int j=i;j+1<a->len;j++)a->data[j]=a->data[j+1];a->len--;return sp_ofloat_of(v);}
static inline sp_int sp_FloatArray_length(sp_FloatArray*a){return a->len;}
static inline sp_bool sp_FloatArray_empty(sp_FloatArray*a){return a->len==0;}
/* see sp_IntArray_get / _elem_nil / _oget */
static inline sp_float sp_FloatArray_get(sp_FloatArray*a,sp_int i){if(!a)return 0.0;if((unsigned long long)i<(unsigned long long)a->len)return a->data[i];if(i<0&&i+a->len>=0)return a->data[i+a->len];return 0.0;}
static inline sp_bool sp_FloatArray_elem_nil(sp_FloatArray*a,sp_int i){return SP_UNLIKELY(a->nilbits!=NULL)&&sp_nilbit_get(a->nilbits,i);}
/* the Float twin of sp_IntArray_oget / _oget_slow */
static SP_NOINLINE SP_COLD sp_ofloat sp_FloatArray_oget_slow(sp_FloatArray*a,sp_int i){if(!a)return sp_ofloat_nil();if(!((unsigned long long)i<(unsigned long long)a->len)){if(i<0)i+=a->len;if(i<0||i>=a->len)return sp_ofloat_nil();}sp_float v=a->data[i];if(a->nilbits&&sp_nilbit_get(a->nilbits,i))return sp_ofloat_nil();return sp_ofloat_of(v);}
static SP_INLINE sp_ofloat sp_FloatArray_oget(sp_FloatArray*a,sp_int i){if(SP_LIKELY(a&&(unsigned long long)i<(unsigned long long)a->len)){sp_float v=a->data[i];if(SP_LIKELY(!a->nilbits))return sp_ofloat_of(v);}return sp_FloatArray_oget_slow(a,i);}
/* The fused reads (`a[i] + 1`, `f(a[i])`): the plain element, CRuby's
   error raised at the read for an index past the end or a nil element.
   The in-range read of an array that never held a nil is one unsigned
   compare, the load, and one bitmap test after it (the load first, so
   the header's data/start pair stays one ldp); the rest (a negative
   index, a bitmap, a nil receiver) is one cold call, the read and its error.
   _ck: the element is a receiver, NoMethodError "undefined method 'op' for nil";
   _arg: the element is an argument, TypeError (sp_oint_arg / sp_ofloat_arg);
   _opnd: the element is an arithmetic op's right operand, TypeError "nil can't
   be coerced into Integer/Float" (sp_oint_opnd / sp_ofloat_opnd). */
static SP_NOINLINE SP_COLD sp_int sp_IntArray_get_ck_slow(sp_IntArray *a, sp_int i, const char *op) { return sp_oint_val(sp_IntArray_oget_slow(a, i), op); }
static SP_NOINLINE SP_COLD sp_int sp_IntArray_get_arg_slow(sp_IntArray *a, sp_int i) { return sp_oint_arg(sp_IntArray_oget_slow(a, i)); }
static SP_NOINLINE SP_COLD sp_float sp_FloatArray_get_ck_slow(sp_FloatArray *a, sp_int i, const char *op) { return sp_ofloat_val(sp_FloatArray_oget_slow(a, i), op); }
static SP_NOINLINE SP_COLD sp_float sp_FloatArray_get_arg_slow(sp_FloatArray *a, sp_int i) { return sp_ofloat_arg(sp_FloatArray_oget_slow(a, i)); }
static SP_NOINLINE SP_COLD sp_int sp_IntArray_get_opnd_slow(sp_IntArray *a, sp_int i) { return sp_oint_opnd(sp_IntArray_oget_slow(a, i)); }
static SP_NOINLINE SP_COLD sp_float sp_FloatArray_get_opnd_slow(sp_FloatArray *a, sp_int i) { return sp_ofloat_opnd(sp_FloatArray_oget_slow(a, i)); }
static SP_INLINE sp_int sp_IntArray_get_ck(sp_IntArray *a, sp_int i, const char *op) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_int v = a->data[a->start + i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_IntArray_get_ck_slow(a, i, op);
}
static SP_INLINE sp_int sp_IntArray_get_arg(sp_IntArray *a, sp_int i) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_int v = a->data[a->start + i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_IntArray_get_arg_slow(a, i);
}
static SP_INLINE sp_float sp_FloatArray_get_ck(sp_FloatArray *a, sp_int i, const char *op) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_float v = a->data[i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_FloatArray_get_ck_slow(a, i, op);
}
static SP_INLINE sp_float sp_FloatArray_get_arg(sp_FloatArray *a, sp_int i) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_float v = a->data[i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_FloatArray_get_arg_slow(a, i);
}
static SP_INLINE sp_int sp_IntArray_get_opnd(sp_IntArray *a, sp_int i) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_int v = a->data[a->start + i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_IntArray_get_opnd_slow(a, i);
}
static SP_INLINE sp_float sp_FloatArray_get_opnd(sp_FloatArray *a, sp_int i) {
  if (SP_LIKELY(a && (unsigned long long)i < (unsigned long long)a->len)) { sp_float v = a->data[i]; if (SP_LIKELY(!a->nilbits)) return v; }
  return sp_FloatArray_get_opnd_slow(a, i);
}
/* the `_o` spelling of the element read with its nil (a builtin row's sp_$AArray_get$O) */
#define sp_IntArray_get_o(a, i) sp_IntArray_oget((a), (i))
#define sp_FloatArray_get_o(a, i) sp_FloatArray_oget((a), (i))
/* first/last: nil when empty, else the element */
static inline sp_ofloat sp_FloatArray_first_opt(sp_FloatArray*a){return (!a||a->len<=0)?sp_ofloat_nil():sp_FloatArray_oget(a,0);}
static inline sp_ofloat sp_FloatArray_last_opt(sp_FloatArray*a){return (!a||a->len<=0)?sp_ofloat_nil():sp_FloatArray_oget(a,a->len-1);}
/* The write at or past the end: slots up to `i` read as nil (their bits
   set). Out of line and cold, as the Integer set_slow is, so the in-range
   store stays small. */
static SP_NOINLINE SP_COLD void sp_FloatArray_fill_to(sp_FloatArray*a,sp_int i){if(i>a->len){if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBITS_SET_RANGE_A(a, a->len, i);}while(i>=a->len){a->data[a->len]=0.0;a->len++;}}
static SP_NOINLINE SP_COLD void sp_FloatArray_clr_nil(sp_FloatArray*a,sp_int i){sp_nilbit_clr(a->nilbits,i);}
/* Issue #769: no-op for negative index after adjustment. */
static inline void sp_FloatArray_set(sp_FloatArray*a,sp_int i,sp_float v){if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->cap)sp_FloatArray_set_cap(a,a->cap*2+1);if(i>=a->len)sp_FloatArray_fill_to(a,i);a->data[i]=v;if(SP_UNLIKELY(a->nilbits))sp_FloatArray_clr_nil(a,i);}  /* the gap is nil, not 0.0 (#3836) */
static SP_NOINLINE SP_COLD void sp_FloatArray_set_nil(sp_FloatArray*a,sp_int i){if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->cap)sp_FloatArray_set_cap(a,a->cap*2+1);if(i>=a->len)sp_FloatArray_fill_to(a,i);a->data[i]=0.0;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, i);}
static inline void sp_FloatArray_oset(sp_FloatArray*a,sp_int i,sp_ofloat o){if(SP_UNLIKELY(o.nil))sp_FloatArray_set_nil(a,i);else sp_FloatArray_set(a,i,o.v);}
static SP_NOINLINE SP_COLD void sp_FloatArray_push_nil(sp_FloatArray*a){sp_FloatArray_push(a,0.0);if(a->frozen)return;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->len-1);}
static inline void sp_FloatArray_push_o(sp_FloatArray*a,sp_ofloat o){if(SP_UNLIKELY(o.nil))sp_FloatArray_push_nil(a);else sp_FloatArray_push(a,o.v);}
/* The Float twins of the IntArray helpers above. */
#define sp_FloatArray_push_nilable(a, o) sp_FloatArray_push_o((a), (o))
#define sp_FloatArray_set_nilable(a, i, o) sp_FloatArray_oset((a), (i), (o))
#define sp_FloatArray_nil_from(d, s) ((void)0)
#define sp_FloatArray_may_nil(a) ({ const sp_FloatArray *_mn_a = (a); (sp_bool)(_mn_a && SP_MAY_NIL(_mn_a)); })
static inline sp_bool sp_FloatArray_has_nil(sp_FloatArray*a){return a&&a->nilbits&&sp_nilbits_any(a->nilbits,0,a->len);}
static inline sp_int sp_FloatArray_nil_count(sp_FloatArray*a){return (a&&a->nilbits)?sp_nilbits_count(a->nilbits,0,a->len):0;}
static SP_COLD void sp_FloatArray_drop_nilbits(sp_FloatArray*a){if(a&&a->nilbits){sp_nilbits_free(a,a->nilbits,a->cap);a->nilbits=NULL;}}
static inline sp_RbVal sp_FloatArray_box_elem(sp_FloatArray*a,sp_int i){if(!a||i<0||i>=a->len||sp_FloatArray_elem_nil(a,i))return sp_box_nil();return sp_box_float(a->data[i]);}

/* One step of CRuby's compensated summation (array.c ary_sum): Kahan-Babuska-
   Neumaier, where `comp` collects the low-order bits each add drops and is
   folded back once the run ends. The special-value arms are CRuby's own: a
   total that is already NaN stays NaN, a NaN element makes it NaN, and an
   Infinity element takes the total over unless it meets an Infinity of the
   other sign, which is NaN. Without them the compensation computed
   (inf - inf) and `[Float::INFINITY, 1.0].sum` answered NaN where Ruby
   answers Infinity. Shared so the typed float sum and the boxed fold in
   spinel_rt.h cannot drift apart. */
static SP_INLINE void sp_float_sum_nonfinite(sp_float *sum, sp_float *comp, sp_float x) {
  sp_float f = *sum;
  if (isnan(f)) return;
  if (isnan(x)) { *sum = x; return; }
  if (isinf(x)) {
    *sum = (isinf(f) && signbit(x) != signbit(f)) ? (sp_float)NAN : x;
    return;
  }
  if (isinf(f)) return;
  {
    sp_float t = f + x;
    if (fabs(f) >= fabs(x)) *comp += (f - t) + x;
    else *comp += (x - t) + f;
    *sum = t;
  }
}
static inline void sp_float_sum_step(sp_float *sum, sp_float *comp, sp_float x) {
  sp_float f = *sum, t = f + x;
  if (SP_UNLIKELY(!isfinite(t))) { sp_float_sum_nonfinite(sum, comp, x); return; }
  if (fabs(f) >= fabs(x)) *comp += (f - t) + x;
  else *comp += (x - t) + f;
  *sum = t;
}

/* ---- sp_FloatArray cold ops (compiled in lib/sp_array.c) ---- */
void sp_FloatArray_unshift(sp_FloatArray *a, sp_float v);
void sp_FloatArray_unshift_nil(sp_FloatArray *a);
sp_FloatArray *sp_FloatArray_from_step(sp_float beg, sp_float end, sp_float step, sp_int excl);
sp_float sp_float_step_size(sp_float beg, sp_float end, sp_float unit, sp_int excl);
/* The i-th value of CRuby's ruby_float_step: computed rather than accumulated,
   clamped to end on overshoot; an infinite unit only ever yields beg. */
static inline sp_float sp_float_step_at(sp_float beg, sp_float end, sp_float unit, sp_int i){if(isinf(unit))return beg;sp_float d=(sp_float)i*unit+beg;if(unit>=0?end<d:d<end)d=end;return d;}
sp_ofloat sp_FloatArray_min_o(sp_FloatArray *a);
sp_ofloat sp_FloatArray_max_o(sp_FloatArray *a);
sp_float sp_FloatArray_sum(sp_FloatArray *a, sp_float init);
void sp_FloatArray_replace(sp_FloatArray *dst, sp_FloatArray *src);
sp_FloatArray *sp_FloatArray_slice(sp_FloatArray *a, sp_int start, sp_int len);
sp_FloatArray *sp_FloatArray_slice_range(sp_FloatArray *a, sp_int start, sp_int end_, sp_int excl);
void sp_FloatArray_reverse_bang(sp_FloatArray *a);
void sp_FloatArray_rotate_bang(sp_FloatArray *a, sp_int n);
void sp_FloatArray_sort_bang(sp_FloatArray *a);
void sp_FloatArray_shuffle_bang(sp_FloatArray *a);
sp_FloatArray *sp_FloatArray_dup(sp_FloatArray *a);
sp_FloatArray *sp_FloatArray_sort(sp_FloatArray *a);
sp_FloatArray *sp_FloatArray_shuffle(sp_FloatArray *a);
sp_ofloat sp_FloatArray_sample_o(sp_FloatArray *a);
sp_bool sp_FloatArray_include(sp_FloatArray *a, sp_float v);
sp_int sp_FloatArray_index(sp_FloatArray *a, sp_float v);
sp_int sp_FloatArray_rindex(sp_FloatArray *a, sp_float v);
sp_ofloat sp_FloatArray_delete_o(sp_FloatArray *a, sp_float v);
sp_bool sp_FloatArray_delete_nil(sp_FloatArray *a);
sp_FloatArray *sp_FloatArray_intersect(sp_FloatArray *a, sp_FloatArray *b);
sp_bool sp_FloatArray_intersect_p(sp_FloatArray *a, sp_FloatArray *b);
sp_FloatArray *sp_FloatArray_union(sp_FloatArray *a, sp_FloatArray *b);
sp_FloatArray *sp_FloatArray_difference(sp_FloatArray *a, sp_FloatArray *b);
sp_FloatArray *sp_FloatArray_uniq(sp_FloatArray *a);
void sp_FloatArray_uniq_bang(sp_FloatArray *a);
void sp_FloatArray_insert(sp_FloatArray *a, sp_int i, sp_float v);
void sp_FloatArray_insert_nil(sp_FloatArray *a, sp_int i);

/* ============================= sp_PtrArray ============================ */
/* Array of void* pointers (user-class arrays, FFI pointer arrays). */
static void sp_PtrArray_fin(void*p){sp_PtrArray*a=(sp_PtrArray*)p;sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes_sub(sizeof(void*)*a->cap);h->size-=sizeof(void*)*a->cap;sp_pl_free(a->data);}
static void sp_PtrArray_gc_scan(void*p){sp_PtrArray*a=(sp_PtrArray*)p;if(!a->scan_elem)return;for(sp_int i=0;i<a->len;i++){if(a->data[i])a->scan_elem(a->data[i]);}}
static sp_PtrArray*sp_PtrArray_new_scan(void(*scan_elem)(void*)){sp_PtrArray*a=(sp_PtrArray*)sp_gc_alloc(sizeof(sp_PtrArray),sp_PtrArray_fin,scan_elem?sp_PtrArray_gc_scan:NULL);a->cap=16;a->data=(void**)sp_pl_alloc(sizeof(void*)*a->cap);if(!a->data)sp_oom_die();a->len=0;a->scan_elem=scan_elem;{sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));h->size+=sizeof(void*)*a->cap;sp_gc_bytes_add(sizeof(void*)*a->cap);}return a;}
static sp_PtrArray*sp_PtrArray_new(void){return sp_PtrArray_new_scan(sp_gc_mark);}
/* PtrArray for raw external pointers (FFI `:ptr` returns, dlopen handles).
   These don't carry sp_gc_hdr -- the default sp_gc_mark element scan would
   read undefined bytes and crash at collection. Skip per-element scanning;
   the array header itself is still GC-tracked. */
static sp_PtrArray*sp_PtrArray_new_noscan(void){return sp_PtrArray_new_scan(NULL);}
static inline void sp_PtrArray_push(sp_PtrArray*a,void*v){sp_gc_wb((void*)a); if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_PTR_ARRAY);return;}if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes_sub(sizeof(void*)*a->cap);h->size-=sizeof(void*)*a->cap;a->cap=((((((a->cap*2))))))+1;void*nd=sp_pl_realloc(a->data,sizeof(void*)*a->cap);if(!nd)sp_oom_die();a->data=(void**)nd;h->size+=sizeof(void*)*a->cap;sp_gc_bytes_add(sizeof(void*)*a->cap);}a->data[a->len++]=v;}
/* Array#pop on a `<X>_ptr_array`. Returns NULL when empty (matches CRuby's
   nil for typed-element arrays since the slot can't carry nil). #520. */
static inline void *sp_PtrArray_pop(sp_PtrArray*a){sp_gc_wb((void*)a); if(!a||a->len==0)return NULL;return a->data[--a->len];}
/* A pointer array boxed by reference (#4486): its elements are what the stamp
   says (sp_box_ptr_array_k), so an element read boxes a row as the typed
   array it is and an object as its own class, and a store takes exactly that
   kind or raises the TypeError the flat typed arrays raise. An array that was
   never stamped (no emitter boxes one without stamping) reads as opaque
   objects, which is what the erased id always meant. Shared by the runtime
   archive and the generated TU, so only sp_alloc.h/sp_gc.h names appear. */
static inline const char *sp_PtrArray_kind_name(sp_PtrArray *a) {
  switch (a->elem_kind) {
    case SP_PTR_ELEM_INT_ROWS: return "Array[Integer]";
    case SP_PTR_ELEM_FLT_ROWS: return "Array[Float]";
    case SP_PTR_ELEM_OBJ: return (a->elem_cls >= 0 && sp_obj_cls_name_fn) ? sp_obj_cls_name_fn(a->elem_cls) : "Object";
    default: return "Object";
  }
}
static inline sp_RbVal sp_PtrArray_elem_box(sp_PtrArray *a, void *e) {
  if (!e) return sp_box_nil();
  switch (a->elem_kind) {
    case SP_PTR_ELEM_INT_ROWS: return sp_box_obj(e, SP_BUILTIN_INT_ARRAY);
    case SP_PTR_ELEM_FLT_ROWS: return sp_box_obj(e, SP_BUILTIN_FLT_ARRAY);
    case SP_PTR_ELEM_OBJ: return sp_box_nullable_obj_dyn(e, 0);   /* the object's own class id, a subclass included */
    default: return sp_box_obj(e, SP_BUILTIN_OBJECT);
  }
}
static inline sp_RbVal sp_PtrArray_get_box(sp_PtrArray *a, sp_int i) {
  if (!a) return sp_box_nil();
  if (i < 0) i += a->len;
  if (i < 0 || i >= a->len) return sp_box_nil();
  return sp_PtrArray_elem_box(a, a->data[i]);
}
/* the class a boxed value would name in a TypeError, from what this header can see */
static inline const char *sp_PtrArray_val_class(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: return "Integer"; case SP_TAG_FLT: return "Float"; case SP_TAG_STR: return "String";
    case SP_TAG_NIL: return "NilClass"; case SP_TAG_SYM: return "Symbol";
    case SP_TAG_BOOL: return v.v.b ? "TrueClass" : "FalseClass";
    case SP_TAG_OBJ:
      if (v.cls_id >= 0) return sp_obj_cls_name_fn ? sp_obj_cls_name_fn(v.cls_id) : "Object";
      switch (v.cls_id) {
        case SP_BUILTIN_STRBUF: return "String";
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_STR_ARRAY:
        case SP_BUILTIN_SYM_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: return "Array";
        default: return "Object";
      }
    default: return "Object";
  }
}
/* can the array hold v? 1 with the pointer in *out (NULL for nil), else 0 */
static inline int sp_PtrArray_elem_ok(sp_PtrArray *a, sp_RbVal v, void **out) {
  *out = NULL;
  if (v.tag == SP_TAG_NIL) return 1;
  if (v.tag != SP_TAG_OBJ) return 0;
  switch (a->elem_kind) {
    case SP_PTR_ELEM_INT_ROWS: if (v.cls_id != SP_BUILTIN_INT_ARRAY) return 0; break;
    case SP_PTR_ELEM_FLT_ROWS: if (v.cls_id != SP_BUILTIN_FLT_ARRAY) return 0; break;
    case SP_PTR_ELEM_OBJ:
      if (!(a->elem_cls < 0 || v.cls_id == a->elem_cls ||
            (v.cls_id >= 0 && sp_class_le_id_fn && sp_class_le_id_fn(v.cls_id, a->elem_cls)))) return 0;
      break;
    default: break;
  }
  *out = v.v.p;
  return 1;
}
static inline void *sp_PtrArray_elem_unbox(sp_PtrArray *a, sp_RbVal v) {
  void *e;
  if (sp_PtrArray_elem_ok(a, v, &e)) return e;
  sp_exc_stage_recv(v);
  sp_raise_cls("TypeError", sp_sprintf("cannot store %s into an Array[%s]: a typed array holds one kind of element",
                                       sp_PtrArray_val_class(v), sp_PtrArray_kind_name(a)));
  return NULL;
}
/* the boxed elements of a pointer array, for the paths that work on a poly array */
static inline sp_PolyArray *sp_PtrArray_to_poly(sp_PtrArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  if (a) for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_PtrArray_elem_box(a, a->data[i]));
  return r;
}
static inline void*sp_PtrArray_get(sp_PtrArray*a,sp_int i){if(!a)return NULL;if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;return a->data[i];}
/* Issue #770: bounds-check the final index; no-op out-of-range rather
   than writing into adjacent memory (typed slots have a fixed shape). */
static inline void sp_PtrArray_set(sp_PtrArray*a,sp_int i,void*v){sp_gc_wb((void*)a); if(!a)return;if(i<0)i+=a->len;if(i<0||i>=a->len)return;a->data[i]=v;}
/* `a[i] = v` through a boxed pointer array: Ruby's index rules (a negative
   index from the end, IndexError below -len, nil-fill past the end), where
   the typed setter above answers a fixed shape (#4486) */
static inline void sp_PtrArray_set_grow(sp_PtrArray*a,sp_int i,void*v){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_PTR_ARRAY);return;}
  sp_int orig=i; if(i<0)i+=a->len;
  if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));
  while(a->len<=i)sp_PtrArray_push(a,NULL);
  sp_gc_wb((void*)a); a->data[i]=v;
}
static inline sp_int sp_PtrArray_length(sp_PtrArray*a){if(!a)return 0;return a->len;}
static inline sp_bool sp_PtrArray_empty(sp_PtrArray*a){sp_gc_wb((void*)a); if(!a)return TRUE;return a->len==0;}

/* ---- sp_PtrArray cold ops (compiled in lib/sp_array.c) ---- */
void *sp_PtrArray_delete_at(sp_PtrArray *a, sp_int i);
void sp_PtrArray_reverse_bang(sp_PtrArray *a);
void sp_PtrArray_rotate_bang(sp_PtrArray *a, sp_int n);
sp_PtrArray *sp_PtrArray_dup(sp_PtrArray *a);
sp_PtrArray *sp_PtrArray_slice(sp_PtrArray *a, sp_int start, sp_int len);
void sp_PtrArray_shuffle_bang(sp_PtrArray *a);
sp_PtrArray *sp_PtrArray_shuffle(sp_PtrArray *a);
void *sp_PtrArray_sample(sp_PtrArray *a);

/* ============================= sp_StrArray ============================ */
/* Small-array optimization: keep the first SP_STRARR_INLINE elements
   inside the struct so empty/short StrArrays skip the data malloc.
   data == inline_data is the discriminator for "still on inline storage". */
static void sp_StrArray_fin(void*p){sp_StrArray*a=(sp_StrArray*)p;if(a->data!=a->inline_data){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes_sub(sizeof(const char*)*a->cap);h->size-=sizeof(const char*)*a->cap;sp_pl_free(a->data);}}
static void sp_StrArray_scan(void*p){sp_StrArray*a=(sp_StrArray*)p;for(sp_int i=0;i<a->len;i++)sp_mark_string(a->data[i]);}
static sp_StrArray*sp_StrArray_new(void){sp_StrArray*a=(sp_StrArray*)sp_gc_alloc(sizeof(sp_StrArray),sp_StrArray_fin,sp_StrArray_scan);a->cap=SP_STRARR_INLINE;a->data=a->inline_data;a->len=0;return a;}
static void sp_StrArray_init_embedded(sp_StrArray*a){a->cap=SP_STRARR_INLINE;a->data=a->inline_data;a->len=0;}  /* see sp_IntArray_init_embedded */
static inline void sp_StrArray_push(sp_StrArray*a,const char*v){sp_gc_wb((void*)a); if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}if(a->len>=a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_int nc=((((((a->cap*2))))))+1;if(a->data==a->inline_data){const char**nd=(const char**)sp_pl_alloc(sizeof(const char*)*nc);if(!nd)sp_oom_die();memcpy(nd,a->data,sizeof(const char*)*a->len);a->data=nd;}
else{sp_gc_bytes_sub(sizeof(const char*)*a->cap);h->size-=sizeof(const char*)*a->cap;void*nd=sp_pl_realloc(a->data,sizeof(const char*)*nc);if(!nd)sp_oom_die();a->data=(const char**)nd;}a->cap=nc;h->size+=sizeof(const char*)*a->cap;sp_gc_bytes_add(sizeof(const char*)*a->cap);}a->data[a->len++]=v;}
static inline sp_int sp_StrArray_length(sp_StrArray*a){return a ? a->len : 0;}
static inline sp_bool sp_StrArray_empty(sp_StrArray*a){sp_gc_wb((void*)a); return a->len==0;}
static inline const char*sp_StrArray_get(sp_StrArray*a,sp_int i){if(!a)return NULL;if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;return a->data[i];}
static inline void sp_StrArray_set(sp_StrArray*a,sp_int i,const char*v){sp_gc_wb((void*)a); if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len));while(i>=a->len)sp_StrArray_push(a,NULL);a->data[i]=v;}  /* the gap is nil, not "" (#3836) */

/* ---- sp_StrArray cold ops (compiled in lib/sp_array.c) ---- */
void sp_StrArray_replace(sp_StrArray *dst, sp_StrArray *src);
const char *sp_StrArray_pop(sp_StrArray *a);
const char *sp_StrArray_shift(sp_StrArray *a);
sp_StrArray *sp_StrArray_slice(sp_StrArray *a, sp_int start, sp_int len);
sp_StrArray *sp_StrArray_slice_range(sp_StrArray *a, sp_int start, sp_int end_, sp_int excl);
void sp_StrArray_reverse_bang(sp_StrArray *a);
void sp_StrArray_rotate_bang(sp_StrArray *a, sp_int n);
void sp_StrArray_sort_bang(sp_StrArray *a);
void sp_StrArray_uniq_bang(sp_StrArray *a);
sp_StrArray *sp_StrArray_uniq(sp_StrArray *a);
const char *sp_StrArray_join(sp_StrArray *a, const char *sep);
sp_bool sp_StrArray_include(sp_StrArray *a, const char *v);
sp_StrArray *sp_StrArray_intersect(sp_StrArray *a, sp_StrArray *b);
sp_bool sp_StrArray_intersect_p(sp_StrArray *a, sp_StrArray *b);
sp_StrArray *sp_StrArray_union(sp_StrArray *a, sp_StrArray *b);
sp_StrArray *sp_StrArray_difference(sp_StrArray *a, sp_StrArray *b);
sp_int sp_StrArray_index(sp_StrArray *a, const char *v);
sp_int sp_StrArray_rindex(sp_StrArray *a, const char *v);
sp_StrArray *sp_StrArray_compact(sp_StrArray *a);
sp_IntArray *sp_IntArray_compact(sp_IntArray *a);
sp_FloatArray *sp_FloatArray_compact(sp_FloatArray *a);
sp_bool sp_IntArray_compact_bang(sp_IntArray *a);
sp_bool sp_FloatArray_compact_bang(sp_FloatArray *a);
sp_IntArray *sp_IntArray_nil_cmp_ck(sp_IntArray *a);
sp_FloatArray *sp_FloatArray_nil_cmp_ck(sp_FloatArray *a);
sp_IntArray *sp_IntArray_nil_sum_ck(sp_IntArray *a, int float_seed);
sp_FloatArray *sp_FloatArray_nil_sum_ck(sp_FloatArray *a, int float_seed);
/* unshift / insert of a value that may be nil (see sp_IntArray_push_o) */
#define sp_IntArray_unshift_o(a, o) ({ sp_IntArray *_un_a = (a); sp_oint _un_o = (o); if (SP_UNLIKELY(_un_o.nil)) sp_IntArray_unshift_nil(_un_a); else sp_IntArray_unshift(_un_a, _un_o.v); })
#define sp_IntArray_insert_o(a, i, o) ({ sp_IntArray *_in_a = (a); sp_int _in_i = (i); sp_oint _in_o = (o); if (SP_UNLIKELY(_in_o.nil)) sp_IntArray_insert_nil(_in_a, _in_i); else sp_IntArray_insert(_in_a, _in_i, _in_o.v); })
#define sp_FloatArray_insert_o(a, i, o) ({ sp_FloatArray *_in_a = (a); sp_int _in_i = (i); sp_ofloat _in_o = (o); if (SP_UNLIKELY(_in_o.nil)) sp_FloatArray_insert_nil(_in_a, _in_i); else sp_FloatArray_insert(_in_a, _in_i, _in_o.v); })
#define sp_FloatArray_unshift_o(a, o) ({ sp_FloatArray *_un_a = (a); sp_ofloat _un_o = (o); if (SP_UNLIKELY(_un_o.nil)) sp_FloatArray_unshift_nil(_un_a); else sp_FloatArray_unshift(_un_a, _un_o.v); })
#define sp_IntArray_unshift_nilable(a, o) sp_IntArray_unshift_o((a), (o))
#define sp_IntArray_insert_nilable(a, i, o) sp_IntArray_insert_o((a), (i), (o))
#define sp_FloatArray_insert_nilable(a, i, o) sp_FloatArray_insert_o((a), (i), (o))
#define sp_FloatArray_unshift_nilable(a, o) sp_FloatArray_unshift_o((a), (o))
/* The elements that are not nil, for all? / any? / none? / one?: the length,
   less the nils the bitmap holds. (`marked` -- analyze saw a nil stored --
   no longer matters: a stored nil always has its bit.) */
#define sp_IntArray_truthy_count(a, marked) ({ sp_IntArray *_tc_a = (a); (void)(marked); !_tc_a ? (sp_int)0 : _tc_a->len - sp_IntArray_nil_count(_tc_a); })
#define sp_FloatArray_truthy_count(a, marked) ({ sp_FloatArray *_tc_a = (a); (void)(marked); !_tc_a ? (sp_int)0 : _tc_a->len - sp_FloatArray_nil_count(_tc_a); })
/* The receiver of min, max, minmax, sort (cmp) or a blockless sum, checked
   for a nil element: one pointer test, and the scan (the _ck above) only
   where a nil was ever stored. */
#define sp_IntArray_nil_cmp_if_flagged(a) ({ sp_IntArray *_ff_a = (a); (_ff_a && SP_MAY_NIL(_ff_a)) ? sp_IntArray_nil_cmp_ck(_ff_a) : _ff_a; })
#define sp_FloatArray_nil_cmp_if_flagged(a) ({ sp_FloatArray *_ff_a = (a); (_ff_a && SP_MAY_NIL(_ff_a)) ? sp_FloatArray_nil_cmp_ck(_ff_a) : _ff_a; })
#define sp_IntArray_nil_sum_if_flagged(a, fs) ({ sp_IntArray *_ff_a = (a); (_ff_a && SP_MAY_NIL(_ff_a)) ? sp_IntArray_nil_sum_ck(_ff_a, (fs)) : _ff_a; })
#define sp_FloatArray_nil_sum_if_flagged(a, fs) ({ sp_FloatArray *_ff_a = (a); (_ff_a && SP_MAY_NIL(_ff_a)) ? sp_FloatArray_nil_sum_ck(_ff_a, (fs)) : _ff_a; })
const char *sp_StrArray_delete_at(sp_StrArray *a, sp_int i);
const char *sp_StrArray_delete(sp_StrArray *a, const char *v);
void sp_StrArray_insert(sp_StrArray *a, sp_int i, const char *v);
void sp_StrArray_shuffle_bang(sp_StrArray *a);
sp_StrArray *sp_StrArray_dup(sp_StrArray *a);
sp_StrArray *sp_StrArray_sort(sp_StrArray *a);
sp_StrArray *sp_StrArray_shuffle(sp_StrArray *a);
const char *sp_StrArray_sample(sp_StrArray *a);

/* The searches with a needle that may itself be nil: a nil needle finds the
   array's nil elements, a value its equals. */
sp_oint sp_IntArray_index_nil(sp_IntArray *a, int rev);
sp_oint sp_FloatArray_index_nil(sp_FloatArray *a, int rev);
static inline sp_bool sp_IntArray_include_o(sp_IntArray *a, sp_oint o) { return o.nil ? sp_IntArray_has_nil(a) : sp_IntArray_include(a, o.v); }
static inline sp_bool sp_FloatArray_include_o(sp_FloatArray *a, sp_ofloat o) { return o.nil ? sp_FloatArray_has_nil(a) : sp_FloatArray_include(a, o.v); }
static inline sp_oint sp_IntArray_index_o(sp_IntArray *a, sp_oint o) { if (o.nil) return sp_IntArray_index_nil(a, 0); { sp_int n = sp_IntArray_index(a, o.v); return n < 0 ? sp_oint_nil() : sp_oint_of(n); } }
static inline sp_oint sp_IntArray_rindex_o(sp_IntArray *a, sp_oint o) { if (o.nil) return sp_IntArray_index_nil(a, 1); { sp_int n = sp_IntArray_rindex(a, o.v); return n < 0 ? sp_oint_nil() : sp_oint_of(n); } }
static inline sp_oint sp_FloatArray_index_o(sp_FloatArray *a, sp_ofloat o) { if (o.nil) return sp_FloatArray_index_nil(a, 0); { sp_int n = sp_FloatArray_index(a, o.v); return n < 0 ? sp_oint_nil() : sp_oint_of(n); } }
static inline sp_oint sp_FloatArray_rindex_o(sp_FloatArray *a, sp_ofloat o) { if (o.nil) return sp_FloatArray_index_nil(a, 1); { sp_int n = sp_FloatArray_rindex(a, o.v); return n < 0 ? sp_oint_nil() : sp_oint_of(n); } }
/* delete(v) with a needle that may be nil: the deleted value (nil for a nil needle, which removes the nils) */
static inline sp_oint sp_IntArray_delete_on(sp_IntArray *a, sp_oint o) { if (o.nil) { sp_IntArray_delete_nil(a); return sp_oint_nil(); } return sp_IntArray_delete_o(a, o.v); }
static inline sp_ofloat sp_FloatArray_delete_on(sp_FloatArray *a, sp_ofloat o) { if (o.nil) { sp_FloatArray_delete_nil(a); return sp_ofloat_nil(); } return sp_FloatArray_delete_o(a, o.v); }
/* count(v) with a needle that may be nil */
static inline sp_int sp_IntArray_count_o(sp_IntArray *a, sp_oint o) { sp_int n = 0; if (!a) return 0; for (sp_int i = 0; i < a->len; i++) { int en = sp_IntArray_elem_nil(a, i); if (o.nil ? en : (!en && a->data[a->start + i] == o.v)) n++; } return n; }
static inline sp_int sp_FloatArray_count_o(sp_FloatArray *a, sp_ofloat o) { sp_int n = 0; if (!a) return 0; for (sp_int i = 0; i < a->len; i++) { int en = sp_FloatArray_elem_nil(a, i); if (o.nil ? en : (!en && (a->data[i] == o.v || (o.v != o.v && a->data[i] != a->data[i])))) n++; } return n; }

/* ---- poly/inspect-dependent ops (lib/sp_array.c; need sp_inspect.h/sp_str.h) ---- */
void sp_str_upto_each(const char *s, const char *e, sp_int excl, int (*fn)(const char *, void *), void *arg);
sp_StrArray *sp_StrArray_from_string_range(const char *s, const char *e, sp_int excl);
const char*sp_IntArray_inspect(sp_IntArray*a);
const char*sp_FloatArray_inspect(sp_FloatArray*a);
const char*sp_FloatArray_join(sp_FloatArray*a,const char*sep);
sp_bool sp_FloatArray_eq(sp_FloatArray*a,sp_FloatArray*b);
const char*sp_StrArray_inspect(sp_StrArray*a);
const char*sp_PtrArray_inspect(sp_PtrArray*a);
sp_PtrArray*sp_IntArray_slice_before(sp_IntArray*a,sp_int d);
sp_PtrArray*sp_IntArray_slice_after(sp_IntArray*a,sp_int d);
sp_PtrArray *sp_IntArray_product(sp_IntArray *a, sp_IntArray *b);
const char*sp_PtrArray_str_join(sp_PtrArray*a,const char*sep);
sp_RbVal sp_IntArray_index_poly(sp_IntArray *a, sp_int v);
sp_RbVal sp_IntArray_rindex_poly(sp_IntArray *a, sp_int v);
sp_RbVal sp_StrArray_index_poly(sp_StrArray *a, const char *v);
sp_RbVal sp_StrArray_rindex_poly(sp_StrArray *a, const char *v);
sp_oint sp_IntArray_index_opt(sp_IntArray *a, sp_int v);
sp_oint sp_IntArray_rindex_opt(sp_IntArray *a, sp_int v);
/* index(nil) / rindex(nil) / include?(nil) / count(nil): the bitmap's view */
sp_oint sp_IntArray_index_nil(sp_IntArray *a, int rev);
sp_oint sp_FloatArray_index_nil(sp_FloatArray *a, int rev);
sp_RbVal sp_FloatArray_index_poly(sp_FloatArray *a, sp_float v);
sp_RbVal sp_FloatArray_rindex_poly(sp_FloatArray *a, sp_float v);
sp_RbVal sp_FloatArray_index_key(sp_FloatArray *a, sp_RbVal v);
sp_RbVal sp_FloatArray_rindex_key(sp_FloatArray *a, sp_RbVal v);
sp_ofloat sp_FloatArray_delete_key(sp_FloatArray *a, sp_RbVal v);
const int64_t *sp_IntArray_ffi_data(sp_IntArray *a);
const double *sp_FloatArray_ffi_data(sp_FloatArray *a);
sp_IntArray *sp_IntArray_concat(sp_IntArray *a, sp_IntArray *b);
sp_StrArray *sp_StrArray_concat(sp_StrArray *a, sp_StrArray *b);
sp_FloatArray *sp_FloatArray_concat(sp_FloatArray *a, sp_FloatArray *b);
sp_PolyArray *sp_IntArray_to_poly(sp_IntArray *a);
sp_PolyArray *sp_StrArray_to_poly_fmt(sp_StrArray *a);
sp_PolyArray *sp_FloatArray_to_poly(sp_FloatArray *a);
sp_IntArray *sp_IntArray_slice_bang(sp_IntArray *a, sp_int from, sp_int n);
sp_FloatArray *sp_FloatArray_slice_bang(sp_FloatArray *a, sp_int from, sp_int n);
sp_StrArray *sp_StrArray_slice_bang(sp_StrArray *a, sp_int from, sp_int n);
sp_PtrArray *sp_PtrArray_slice_bang(sp_PtrArray *a, sp_int from, sp_int n);

/* ---- more cold StrArray ops relocated from spinel_rt.h (0 optcarrot
   uses). #include here (not near the top): by this point array.h's own
   hot inline core (sp_StrArray_push et al) is already defined, so
   sp_str.h's nested processing (it needs sp_StrArray_push for
   sp_str_split_push) sees it; the reverse order undeclares it. ---- */
#include "sp_str.h"     /* sp_str_eq, for the cold StrArray ops below */
const char *sp_StrArray_sum_str(sp_StrArray *a, const char *init);
sp_RbVal sp_StrArray_uniq_bangq(sp_StrArray *a);
sp_bool sp_StrArray_eq(sp_StrArray*a,sp_StrArray*b);

/* ---- sp_typed_arr_frozen relocated from spinel_rt.h (0 optcarrot uses). ---- */
int sp_typed_arr_frozen(sp_RbVal v);

#endif /* SP_ARRAY_H */
