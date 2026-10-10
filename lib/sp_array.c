/* sp_array.c -- cold typed-array operations (see sp_array.h).
 *
 * The hot accessors (new / push / pop / shift / get / set / length /
 * empty) are inline in sp_array.h and compiled per generated TU. The
 * out-of-line ops below -- sort / slice / dup / set algebra / join /
 * inspect-free reductions -- are cold (never on optcarrot's inner loop),
 * so they compile once into libspinel_rt.a instead of into every TU.
 *
 * They reach the GC string heap via sp_alloc.h and call back into the
 * inline core through sp_array.h; sp_sprintf / sp_raise_* are resolved
 * at the final link against the generated TU. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stdint.h>
#include "sp_array.h"
#include "sp_random.h"  /* sp_krand_below for shuffle/sample */
#include "sp_string.h"   /* sp_String builder for join/to_poly */
#include "sp_inspect.h"  /* sp_inspect_container for the #inspect wrappers */
#include "sp_str.h"      /* sp_str_succ for from_string_range */

/* Issue #799: clamp e-s+1 against size_t overflow + an arbitrary sanity
   cap (1 << 30 elements; ~8 GB at 8 bytes/elem). Without the cap,
   `(1..INTPTR_MAX).to_a` overflows the realloc size_t to a tiny number,
   then writes past the allocation. */
sp_IntArray*sp_IntArray_from_range(sp_int s,sp_int e){sp_IntArray*a=sp_IntArray_new();/* the span in unsigned: e - s overflowed for bounds at the ends of sp_int */sp_int n=e<s?0:((uintptr_t)e-(uintptr_t)s>=(uintptr_t)(1LL<<30)?(sp_int)(1LL<<30):(sp_int)((uintptr_t)e-(uintptr_t)s)+1);if(n>(sp_int)(1LL<<30))n=(sp_int)(1LL<<30);if(n>a->cap)sp_IntArray_set_cap(a,n);for(sp_int i=0;i<n;i++)a->data[i]=s+i;a->len=n;return a;}
/* ---- nil-bitmap helpers for the cold ops ----
   An array that never held a nil keeps nilbits NULL and takes the plain
   paths below unchanged. One that did is handled in one of two ways: the
   bits of a copied span follow it (ia_bits_copy), or -- for the ops that
   permute or filter in place -- the window is read out as (value, nil)
   pairs, worked on, and written back with its bits (ia_pack / ia_unpack). */
static void ia_bits_copy(sp_IntArray*d,sp_int dpos,sp_IntArray*s,sp_int spos,sp_int n){if(!s||!s->nilbits||!sp_nilbits_any(s->nilbits,s->start+spos,s->start+spos+n))return;if(!d->nilbits)SP_NILBITS_NEW_A(d);for(sp_int i=0;i<n;i++)if(sp_nilbit_get(s->nilbits,s->start+spos+i))SP_NILBIT_SET_A(d, d->start+dpos+i);}
static sp_oint*ia_pack(sp_IntArray*a){sp_oint*p=(sp_oint*)sp_pl_alloc(sizeof(sp_oint)*(size_t)(a->len>0?a->len:1));if(!p)sp_oom_die();for(sp_int i=0;i<a->len;i++)p[i]=sp_IntArray_oget(a,i);return p;}
/* n <= the window's length: the pairs become the window, the rest of the old window is dropped */
static void ia_unpack(sp_IntArray*a,sp_oint*p,sp_int n){if(a->nilbits)sp_nilbits_clear_range(a->nilbits,a->start,a->start+a->len);for(sp_int i=0;i<n;i++){a->data[a->start+i]=p[i].v;if(p[i].nil){if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->start+i);}}a->len=n;sp_pl_free(p);}
static sp_ofloat*fa_pack(sp_FloatArray*a){sp_ofloat*p=(sp_ofloat*)sp_pl_alloc(sizeof(sp_ofloat)*(size_t)(a->len>0?a->len:1));if(!p)sp_oom_die();for(sp_int i=0;i<a->len;i++)p[i]=sp_FloatArray_oget(a,i);return p;}
static int ia_oeq(sp_oint x,sp_oint y){return x.nil?y.nil:(!y.nil&&x.v==y.v);}
static sp_bool ia_include_o(sp_IntArray*a,sp_oint o){return o.nil?sp_IntArray_has_nil(a):sp_IntArray_include(a,o.v);}
/* (beg..end).step(step) materialised as an IntArray. step==0 raises like
   CRuby; a negative step descends; exclusive ranges drop the endpoint. The
   loop tests the bound directly and guards v+=step, so a range near the
   sp_int limits (e.g. an endless range's INTPTR_MAX end) cannot overflow --
   signed integer overflow is undefined behaviour in C. */
sp_IntArray*sp_IntArray_from_range_step(sp_int beg,sp_int end,sp_int step,sp_int excl){
  if(step==0)sp_raise_cls("ArgumentError","step can't be 0");
  /* An endless range reaches here as INTPTR_MAX, so the loop below walks the
     whole integer line and the process hangs. The float variant already
     refuses a span it cannot materialise; do the same here, counting in
     unsigned arithmetic so the span of an endless range cannot overflow.
     CRuby answers a lazy sequence, which spinel does not have. */
  { int forward = step > 0 ? (beg < end || (!excl && beg == end))
                           : (beg > end || (!excl && beg == end));
    uintptr_t span = step > 0 ? (uintptr_t)end - (uintptr_t)beg
                             : (uintptr_t)beg - (uintptr_t)end;
    uintptr_t mag  = step > 0 ? (uintptr_t)step : -(uintptr_t)step;
    if(forward && span / mag >= (uintptr_t)(1LL<<30))
      sp_raise_cls("RangeError","range too large to materialize"); }
  sp_IntArray*a=sp_IntArray_new();
  if(step>0){
    for(sp_int v=beg;v<end||(!excl&&v==end);){
      sp_IntArray_push(a,v);
      if(v>0&&step>INTPTR_MAX-v)break;
      v+=step;
    }
  }
  else{
    for(sp_int v=beg;v>end||(!excl&&v==end);){
      sp_IntArray_push(a,v);
      if(v<0&&step<INTPTR_MIN-v)break;
      v+=step;
    }
  }
  return a;
}
sp_IntArray*sp_IntArray_dup(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*b=sp_IntArray_new();if(a->len>b->cap)sp_IntArray_set_cap(b,a->len);memcpy(b->data,a->data+a->start,sizeof(sp_int)*a->len);b->len=a->len;ia_bits_copy(b,0,a,0,a->len);return b;}
/* a[start, len] / a[start..end] for IntArray. Negative start counts from
   the end. start past the array length yields an empty result; len is
   clamped so we never read past the source. CRuby returns nil for
   out-of-bounds start; we return an empty IntArray since this typed
   collection has no nullable form. */
/* Slice length clamp, shared shape across every *_slice below and in
   sp_str.c / spinel_rt.h: written as `len > N - start`, never
   `start + len > N`. Ruby hands the length straight through from user code,
   so `a[1, 2**63-1]` reaches here with len == INT64_MAX; the sum then
   overflows, wraps negative, the guard reads FALSE, and the clamp is skipped
   entirely. The copy loop after it is bounded only by len, so it walks off the
   heap -- a segfault today on ordinary -O2 for an index CRuby answers fine.
   By this point start is already known to be in [0, N), so N - start cannot
   underflow. */
sp_IntArray*sp_IntArray_slice(sp_IntArray*a,sp_int start,sp_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_IntArray*b=sp_IntArray_new();if(start>=a->len||len<=0)return b;if(len>a->len-start)len=a->len-start;if(len>b->cap)sp_IntArray_set_cap(b,len);memcpy(b->data,a->data+a->start+start,sizeof(sp_int)*len);b->len=len;ia_bits_copy(b,0,a,start,len);return b;}
/* a[start..end] / a[start...end] with possibly negative endpoints.
   Normalize end against a->len first; the bare _slice already handles
   negative start. Issue #496. */
sp_IntArray*sp_IntArray_slice_range(sp_IntArray*a,sp_int start,sp_int end_,sp_int excl){SP_GC_ROOT(a);if(end_<0)end_+=a->len;if(start<0)start+=a->len;if(start<0||start>a->len)return NULL;/* a start outside [-len, len] is nil (#4524); start == len is [] */sp_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_IntArray_slice(a,start,n);}
/* `a.replace(a)` is a no-op in Ruby. Truncating first and copying after made
   it empty the array instead -- the source is the destination, so its length
   was already zero by the time the copy read it. The poly form takes the same
   guard.

   And a FROZEN receiver raises before the truncation, not after: every other
   mutator here checks, but replace did not, so `[1, 2].freeze.replace([3, 4])`
   quietly rewrote it. */
void sp_IntArray_replace(sp_IntArray*dst,sp_IntArray*src){if(dst==src)return;if(dst->frozen){sp_raise_frozen_array_at(dst,SP_BUILTIN_INT_ARRAY);return;}dst->len=0;dst->start=0;sp_IntArray_drop_nilbits(dst);if(src->len>dst->cap)sp_IntArray_set_cap(dst,src->len);memcpy(dst->data,src->data+src->start,sizeof(sp_int)*src->len);dst->len=src->len;ia_bits_copy(dst,0,src,0,src->len);}
/* The cold start-past-the-end splice: nil up to `s`, then the source. Kept
   apart so the splice's in-range path below is unchanged. */
static SP_NOINLINE SP_COLD void sp_IntArray_splice_gap(sp_IntArray*a,sp_int s,const sp_int*src,sp_int srcn){
  SP_GC_ROOT(a);
  sp_int*sb=NULL;
  if(srcn>0){sb=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(sp_int)*(size_t)srcn);}
  while(a->len<s)sp_IntArray_push_nil(a);
  for(sp_int i=0;i<srcn;i++)sp_IntArray_push(a,sb[i]);
  sp_pl_free(sb);
}
static SP_NOINLINE SP_COLD void sp_FloatArray_splice_gap(sp_FloatArray*a,sp_int s,const sp_float*src,sp_int srcn){
  SP_GC_ROOT(a);
  sp_float*sb=NULL;
  if(srcn>0){sb=(sp_float*)sp_pl_alloc(sizeof(sp_float)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(sp_float)*(size_t)srcn);}
  while(a->len<s)sp_FloatArray_push_nil(a);
  for(sp_int i=0;i<srcn;i++)sp_FloatArray_push(a,sb[i]);
  sp_pl_free(sb);
}
/* arr[start,len] = src / arr[range] = src : remove `len` elements at `start`
   and insert the `srcn` elements of `src` in their place, shifting the tail.
   Reuses push so capacity growth + GC byte accounting stay in one place. src
   is snapshotted first so splicing an array into itself (`a[1,1]=a`) is safe.
   A start beyond the end pads the gap with nil (a set bit, or NULL in a
   String array) as CRuby does (the *_splice_gap helpers). */
void sp_IntArray_splice(sp_IntArray*a,sp_int start,sp_int len,const sp_int*src,sp_int srcn){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}
  /* The equal-length overwrite below allocates nothing, so it needs no root --
     and it is the hot shape (optcarrot's per-tile `@bg_pixels[x, 8] = row`).
     Root only on the way into the general path, which does allocate. */
  sp_int alen=a->len,s=start;
  if(s<0)s+=alen;
  if(len<0){sp_raise_cls("IndexError",sp_sprintf("negative length (%lld)",(long long)len));return;}
  if(s<0){sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)start,(long long)-alen));return;}
  if(s>alen){sp_IntArray_splice_gap(a,s,src,srcn);return;}
  if(s+len>alen)len=alen-s;
  /* Equal-length replacement is a pure overwrite: no length change, no
     capacity growth, no tail shift. memmove, since src may alias a's own
     buffer (self-splice). This is the hot shape (optcarrot's per-tile
     `@bg_pixels[x, 8] = <8-elem row>`); the general path below allocates.
     The source holds no nil (it is plain values), so the span's bits clear. */
  if(len==srcn){if(srcn>0)memmove(a->data+a->start+s,src,sizeof(sp_int)*(size_t)srcn);if(SP_UNLIKELY(a->nilbits))sp_nilbits_clear_range(a->nilbits,a->start+s,a->start+s+srcn);return;}
  SP_GC_ROOT(a);
  sp_int*sb=NULL;
  if(srcn>0){sb=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(sp_int)*(size_t)srcn);}
  sp_int tail_from=s+len,tail_n=alen-tail_from;
  sp_int*tb=NULL;
  if(tail_n>0){tb=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)tail_n);if(!tb){sp_pl_free(sb);sp_oom_die();}memcpy(tb,a->data+a->start+tail_from,sizeof(sp_int)*(size_t)tail_n);}
  /* the tail's bits ride along the same way, and the cut span's are dropped */
  sp_oint*to=NULL;
  if(a->nilbits){if(tail_n>0){to=ia_pack(a)+tail_from;}sp_nilbits_clear_range(a->nilbits,a->start+s,a->start+alen);}
  a->len=s;
  for(sp_int i=0;i<srcn;i++)sp_IntArray_push(a,sb[i]);
  for(sp_int i=0;i<tail_n;i++)sp_IntArray_push(a,tb[i]);
  if(to){for(sp_int i=0;i<tail_n;i++)if(to[i].nil)SP_NILBIT_SET_A(a, a->start+s+srcn+i);sp_pl_free(to-tail_from);}
  sp_pl_free(sb);sp_pl_free(tb);
}
/* the splice of a whole typed array, whose nil elements come along */
void sp_IntArray_splice_o(sp_IntArray*a,sp_int start,sp_int len,sp_IntArray*src){
  SP_GC_ROOT(a);SP_GC_ROOT(src);
  if(!src||!src->nilbits){sp_IntArray_splice(a,start,len,src?src->data+src->start:NULL,src?src->len:0);return;}
  sp_oint*p=ia_pack(src);sp_int n=src->len;
  sp_int*vals=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)(n>0?n:1));if(!vals)sp_oom_die();
  for(sp_int i=0;i<n;i++)vals[i]=p[i].v;
  sp_int alen=a?a->len:0,s=start;if(s<0)s+=alen;
  sp_IntArray_splice(a,start,len,vals,n);
  if(a&&!a->frozen&&s>=0){if(!a->nilbits)SP_NILBITS_NEW_A(a);for(sp_int i=0;i<n;i++)if(p[i].nil)SP_NILBIT_SET_A(a, a->start+s+i);}
  sp_pl_free(vals);sp_pl_free(p);
}
void sp_FloatArray_splice(sp_FloatArray*a,sp_int start,sp_int len,const sp_float*src,sp_int srcn){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}
  sp_int alen=a->len,s=start;
  if(s<0)s+=alen;
  if(len<0){sp_raise_cls("IndexError",sp_sprintf("negative length (%lld)",(long long)len));return;}
  if(s<0){sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)start,(long long)-alen));return;}
  if(s>alen){sp_FloatArray_splice_gap(a,s,src,srcn);return;}
  if(s+len>alen)len=alen-s;
  if(len==srcn){if(srcn>0)memmove(a->data+s,src,sizeof(sp_float)*(size_t)srcn);if(SP_UNLIKELY(a->nilbits))sp_nilbits_clear_range(a->nilbits,s,s+srcn);return;}  /* see the int form */
  SP_GC_ROOT(a);
  sp_float*sb=NULL;
  if(srcn>0){sb=(sp_float*)sp_pl_alloc(sizeof(sp_float)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(sp_float)*(size_t)srcn);}
  sp_int tail_from=s+len,tail_n=alen-tail_from;
  sp_float*tb=NULL;
  if(tail_n>0){tb=(sp_float*)sp_pl_alloc(sizeof(sp_float)*(size_t)tail_n);if(!tb){sp_pl_free(sb);sp_oom_die();}memcpy(tb,a->data+tail_from,sizeof(sp_float)*(size_t)tail_n);}
  sp_ofloat*to=NULL;
  if(a->nilbits){if(tail_n>0){to=fa_pack(a)+tail_from;}sp_nilbits_clear_range(a->nilbits,s,alen);}
  a->len=s;
  for(sp_int i=0;i<srcn;i++)sp_FloatArray_push(a,sb[i]);
  for(sp_int i=0;i<tail_n;i++)sp_FloatArray_push(a,tb[i]);
  if(to){for(sp_int i=0;i<tail_n;i++)if(to[i].nil)SP_NILBIT_SET_A(a, s+srcn+i);sp_pl_free(to-tail_from);}
  sp_pl_free(sb);sp_pl_free(tb);
}
void sp_FloatArray_splice_o(sp_FloatArray*a,sp_int start,sp_int len,sp_FloatArray*src){
  SP_GC_ROOT(a);SP_GC_ROOT(src);
  if(!src||!src->nilbits){sp_FloatArray_splice(a,start,len,src?src->data:NULL,src?src->len:0);return;}
  sp_ofloat*p=fa_pack(src);sp_int n=src->len;
  sp_float*vals=(sp_float*)sp_pl_alloc(sizeof(sp_float)*(size_t)(n>0?n:1));if(!vals)sp_oom_die();
  for(sp_int i=0;i<n;i++)vals[i]=p[i].v;
  sp_int alen=a?a->len:0,s=start;if(s<0)s+=alen;
  sp_FloatArray_splice(a,start,len,vals,n);
  if(a&&!a->frozen&&s>=0){if(!a->nilbits)SP_NILBITS_NEW_A(a);for(sp_int i=0;i<n;i++)if(p[i].nil)SP_NILBIT_SET_A(a, s+i);}
  sp_pl_free(vals);sp_pl_free(p);
}
/* A String array's nil is NULL, as a past-the-end `a[i] = s` pads it. The
   source strings stay reachable through the caller's array: a is not
   truncated here, so the raw snapshot needs no rooted holder. */
static SP_NOINLINE SP_COLD void sp_StrArray_splice_gap(sp_StrArray*a,sp_int s,const char*const*src,sp_int srcn){
  SP_GC_ROOT(a);
  const char**sb=NULL;
  if(srcn>0){sb=(const char**)sp_pl_alloc(sizeof(const char*)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(const char*)*(size_t)srcn);}
  while(a->len<s)sp_StrArray_push(a,NULL);
  for(sp_int i=0;i<srcn;i++)sp_StrArray_push(a,sb[i]);
  sp_pl_free(sb);
}
void sp_StrArray_splice(sp_StrArray*a,sp_int start,sp_int len,const char*const*src,sp_int srcn){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}
  sp_int alen=a->len,s=start;
  if(s<0)s+=alen;
  if(len<0){sp_raise_cls("IndexError",sp_sprintf("negative length (%lld)",(long long)len));return;}
  if(s<0){sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)start,(long long)-alen));return;}
  if(s>alen){sp_StrArray_splice_gap(a,s,src,srcn);return;}
  if(s+len>alen)len=alen-s;
  if(len==srcn){if(srcn>0)memmove(a->data+s,src,sizeof(const char*)*(size_t)srcn);return;}  /* see the int form */
  SP_GC_ROOT(a);
  const char**sb=NULL;
  if(srcn>0){sb=(const char**)sp_pl_alloc(sizeof(const char*)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,src,sizeof(const char*)*(size_t)srcn);}
  /* Snapshot the tail into a *rooted* holder before truncating a->len: once
     a->len shrinks, a no longer scans the tail, so a raw buffer would leave the
     tail's GC strings unrooted across the pushes below (which can collect). The
     root sits at function scope -- not inside the guard -- so it stays live for
     the whole splice. */
  sp_int tail_from=s+len,tail_n=alen-tail_from;
  sp_StrArray*tb=NULL;SP_GC_ROOT(tb);
  if(tail_n>0){tb=sp_StrArray_new();for(sp_int i=0;i<tail_n;i++)sp_StrArray_push(tb,a->data[tail_from+i]);}
  a->len=s;
  for(sp_int i=0;i<srcn;i++)sp_StrArray_push(a,sb[i]);
  if(tb)for(sp_int i=0;i<tb->len;i++)sp_StrArray_push(a,tb->data[i]);
  sp_pl_free(sb);
}
/* poly-array splice: like the typed forms but elements are boxed, so a nil gap
   (start past the end) is filled with nil rather than raising. Ruby's `a[s,l]=`
   splices the RHS's elements when it is an Array, else inserts it as one
   element -- a runtime fact decided here from src's class id. */
void sp_PolyArray_splice(sp_PolyArray*a,sp_int start,sp_int len,sp_RbVal src){
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_POLY_ARRAY);return;}
  SP_GC_ROOT(a);
  /* src rooted too: the pushes below can trigger a collection while some of
     the snapshotted elements are reachable only through src. */
  SP_GC_ROOT_RBVAL(src);
  sp_int alen=a->len,s=start;
  if(s<0)s+=alen;
  if(len<0){sp_raise_cls("IndexError",sp_sprintf("negative length (%lld)",(long long)len));return;}
  if(s<0){sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)start,(long long)-alen));return;}
  /* Equal-length replacement inside the array is a pure overwrite, as in the
     typed splices above: no snapshot buffer, no tail copy, no pushes. It was
     ~190x the typed arrays' cost on `colors[x, 8] = chunk` (#4841). One
     barrier covers the stores, as sp_PolyArray_set takes one; a poly source
     may be this very array, hence memmove. */
  if(len>0&&s+len<=alen&&src.tag==SP_TAG_OBJ&&src.v.p){
    sp_RbVal*dst=a->data+s;
    switch(src.cls_id){
      case SP_BUILTIN_POLY_ARRAY:{sp_PolyArray*x=(sp_PolyArray*)src.v.p;if(x->len==len){sp_gc_wb((void*)a);memmove(dst,x->data,sizeof(sp_RbVal)*(size_t)len);return;}break;}
      case SP_BUILTIN_INT_ARRAY:{sp_IntArray*x=(sp_IntArray*)src.v.p;if(x->len==len){sp_gc_wb((void*)a);for(sp_int i=0;i<len;i++)dst[i]=sp_IntArray_box_elem(x,i);return;}break;}
      case SP_BUILTIN_FLT_ARRAY:{sp_FloatArray*x=(sp_FloatArray*)src.v.p;if(x->len==len){sp_gc_wb((void*)a);for(sp_int i=0;i<len;i++)dst[i]=sp_FloatArray_box_elem(x,i);return;}break;}
      case SP_BUILTIN_STR_ARRAY:{sp_StrArray*x=(sp_StrArray*)src.v.p;if(x->len==len){sp_gc_wb((void*)a);for(sp_int i=0;i<len;i++)dst[i]=sp_box_str(x->data[i]);return;}break;}
      default:break;
    }
  }
  /* snapshot the source elements as boxed values (an Integer or Float
     array's nil element boxes as nil, here and in the overwrite above). src's class id decides array-vs-single-element (Ruby
     splices an Array RHS, inserts anything else). A user object with to_ary
     is coerced at COMPILE time when its static type is known; a to_ary
     object reaching here as a runtime poly value still inserts as one
     element -- closing that would need a codegen-installed dispatch hook
     (the sp_obj_cmp_hook pattern). */
  int src_is_array=src.tag==SP_TAG_OBJ&&(src.cls_id==SP_BUILTIN_INT_ARRAY||src.cls_id==SP_BUILTIN_FLT_ARRAY||src.cls_id==SP_BUILTIN_STR_ARRAY||src.cls_id==SP_BUILTIN_POLY_ARRAY||src.cls_id==SP_BUILTIN_PTR_ARRAY);
  sp_int srcn=0;sp_RbVal*sb=NULL;
  if(src_is_array){
    void*p=src.v.p;
    switch(src.cls_id){
      case SP_BUILTIN_INT_ARRAY:{sp_IntArray*x=(sp_IntArray*)p;srcn=x->len;if(srcn>0){sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal)*(size_t)srcn);if(!sb)sp_oom_die();for(sp_int i=0;i<srcn;i++)sb[i]=sp_IntArray_box_elem(x,i);}break;}
      case SP_BUILTIN_FLT_ARRAY:{sp_FloatArray*x=(sp_FloatArray*)p;srcn=x->len;if(srcn>0){sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal)*(size_t)srcn);if(!sb)sp_oom_die();for(sp_int i=0;i<srcn;i++)sb[i]=sp_FloatArray_box_elem(x,i);}break;}
      case SP_BUILTIN_STR_ARRAY:{sp_StrArray*x=(sp_StrArray*)p;srcn=x->len;if(srcn>0){sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal)*(size_t)srcn);if(!sb)sp_oom_die();for(sp_int i=0;i<srcn;i++)sb[i]=sp_box_str(x->data[i]);}break;}
      case SP_BUILTIN_POLY_ARRAY:{sp_PolyArray*x=(sp_PolyArray*)p;srcn=x->len;if(srcn>0){sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal)*(size_t)srcn);if(!sb)sp_oom_die();memcpy(sb,x->data,sizeof(sp_RbVal)*(size_t)srcn);}break;}
      case SP_BUILTIN_PTR_ARRAY:{sp_PtrArray*x=(sp_PtrArray*)p;srcn=x->len;if(srcn>0){sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal)*(size_t)srcn);if(!sb)sp_oom_die();for(sp_int i=0;i<srcn;i++)sb[i]=sp_PtrArray_elem_box(x,x->data[i]);}break;}   /* rows or objects (#4486) */
      default:break;
    }
  }
  else{
    srcn=1;sb=(sp_RbVal*)sp_pl_alloc(sizeof(sp_RbVal));if(!sb)sp_oom_die();sb[0]=src;
  }
  /* clamp/gap: when start is past the end, the [s+len) tail is empty and the
     [alen,s) gap fills with nil */
  sp_int gap=0,tail_from,tail_n;
  if(s<=alen){if(s+len>alen)len=alen-s;tail_from=s+len;tail_n=alen-tail_from;}
  else{gap=s-alen;tail_from=alen;tail_n=0;}
  /* Snapshot the tail into a *rooted* holder before truncating a->len: the tail
     elements belong to a and, once a->len shrinks, are reachable through neither
     a nor src, so a raw buffer would leave them unrooted across the pushes below
     (which can collect). The source snapshot sb needs no such holder -- its
     elements stay reachable through the already-rooted src. */
  sp_PolyArray*tb=NULL;SP_GC_ROOT(tb);
  if(tail_n>0){tb=sp_PolyArray_new();for(sp_int i=0;i<tail_n;i++)sp_PolyArray_push(tb,a->data[tail_from+i]);}
  a->len=(s<=alen)?s:alen;
  for(sp_int i=0;i<gap;i++)sp_PolyArray_push(a,sp_box_nil());
  for(sp_int i=0;i<srcn;i++)sp_PolyArray_push(a,sb[i]);
  if(tb)for(sp_int i=0;i<tb->len;i++)sp_PolyArray_push(a,tb->data[i]);
  sp_pl_free(sb);
}
void sp_IntArray_reverse_bang(sp_IntArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}if(SP_UNLIKELY(a->nilbits)){sp_oint*p=ia_pack(a);for(sp_int i=0,j=a->len-1;i<j;i++,j--){sp_oint t=p[i];p[i]=p[j];p[j]=t;}ia_unpack(a,p,a->len);return;}for(sp_int i=0,j=a->len-1;i<j;i++,j--){sp_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
/* Array#rotate!: move the first n elements to the end. Two block moves --
   save the smaller side, slide the rest, write the saved side back -- rather
   than the triple-reverse's ~1.5n element swaps. The saved side is at most
   half the array, so a small stack buffer covers the sizes that matter (a
   16-element PPU pixel row is rotated once per tile fetch). */
void sp_IntArray_rotate_bang(sp_IntArray*a,sp_int n){SP_GC_ROOT(a);
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}
  if(a->len<=0)return;
  /* already in range is the usual count (a window rotated by part of its
     length): two divisions per call otherwise, on every tile */
  if(n<0||n>=a->len)n=((n%a->len)+a->len)%a->len;
  if(n==0)return;
  if(SP_UNLIKELY(a->nilbits)){sp_oint*p=ia_pack(a);sp_oint*q=(sp_oint*)sp_pl_alloc(sizeof(sp_oint)*(size_t)a->len);if(!q)sp_oom_die();for(sp_int i=0;i<a->len;i++)q[i]=p[(i+n)%a->len];sp_pl_free(p);ia_unpack(a,q,a->len);return;}
  /* Rotating left by n is "the first n elements move to the end", and the
     window already carries its own start offset (the same one push_grow slides
     home). So it is one copy of n words and an index bump, where the in-place
     form below costs a copy of n, a move of len-n, and a copy back -- three
     times the traffic for optcarrot's 16-word pixel window rotated by eight,
     twice per tile. Headroom past the window is what pays for it: grow once to
     hold a few rotations, and slide home when it runs out, which amortizes to
     well under the old cost. */
  if(a->start+a->len+n>a->cap && a->len+n<=a->cap && a->start>0){
    memmove(a->data,a->data+a->start,sizeof(sp_int)*(size_t)a->len);
    a->start=0;
  }
  /* Only grow for a window small enough that the headroom is cheap and the
     rotation is plausibly repeated (a pixel window, a ring of recent values).
     A one-shot rotate of a large array would pay a doubled buffer for a copy
     it never repeats, so that keeps the in-place form. */
  if(a->start+a->len+n>a->cap && a->len+n>a->cap && a->len<=256){
    sp_int nc=a->len*2+n;
    sp_int*nd=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)nc);
    if(nd){
      memcpy(nd,a->data+a->start,sizeof(sp_int)*(size_t)a->len);
      sp_pl_free(a->data); a->data=nd; a->start=0; a->cap=nc;
    }
  }
  if(a->start+a->len+n<=a->cap){
    memcpy(a->data+a->start+a->len,a->data+a->start,sizeof(sp_int)*(size_t)n);
    a->start+=n;
    return;
  }
  sp_int*d=a->data+a->start;
  sp_int rest=a->len-n;
  sp_int keep=n<rest?n:rest;          /* the side we buffer */
  sp_int stackbuf[32];
  sp_int*t=keep<=32?stackbuf:(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)keep);
  if(!t)sp_oom_die();
  if(n<=rest){                          /* buffer the head, slide the tail down */
    memcpy(t,d,sizeof(sp_int)*(size_t)n);
    memmove(d,d+n,sizeof(sp_int)*(size_t)rest);
    memcpy(d+rest,t,sizeof(sp_int)*(size_t)n);
  }
  else{                                 /* buffer the tail, slide the head up */
    memcpy(t,d+n,sizeof(sp_int)*(size_t)rest);
    memmove(d+rest,d,sizeof(sp_int)*(size_t)n);
    memcpy(d,t,sizeof(sp_int)*(size_t)rest);
  }
  if(t!=stackbuf)sp_pl_free(t);
}
static int _sp_int_cmp(const void*a,const void*b){sp_int va=*(const sp_int*)a,vb=*(const sp_int*)b;return(va>vb)-(va<vb);}
/* sort with a nil element: nil against a number is CRuby's ArgumentError
   (sp_IntArray_nil_cmp_ck), and an array of nils alone is already sorted */
static int ia_sort_nils(sp_IntArray*a){if(!a->nilbits)return 0;sp_IntArray_nil_cmp_ck(a);return sp_IntArray_has_nil(a);}
sp_IntArray*sp_IntArray_sort(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*b=sp_IntArray_dup(a);if(ia_sort_nils(b))return b;qsort(b->data+b->start,b->len,sizeof(sp_int),_sp_int_cmp);return b;}
void sp_IntArray_sort_bang(sp_IntArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}if(ia_sort_nils(a))return;qsort(a->data+a->start,a->len,sizeof(sp_int),_sp_int_cmp);}
/* The Array set operations (uniq, uniq!, &, |, -, intersect?) asked
   include? of a growing or a foreign array once per element: quadratic, so
   `uniq` of 100,000 Integers made billions of compares where CRuby, which
   hashes past 16 elements, makes 100,000 probes. Past SP_SETOP_LINEAR
   elements they now probe an open-addressed set; below it the scan, cheaper
   than building one, stays. The answers do not change: the first occurrence
   is kept, in the receiver's order, by the same equality as before. The table
   is malloc'd and holds nothing the collector must see -- Integers are
   values, and a String in it is also in an array the operation roots. */
#define SP_SETOP_LINEAR 16
typedef struct { sp_int *k; unsigned char *used; size_t mask; } sp_iset;
static void sp_iset_init(sp_iset *t, sp_int n) {
  size_t cap = 32;
  while (cap < (size_t)n * 2) cap <<= 1;
  t->k = (sp_int *)malloc(cap * sizeof(sp_int));
  t->used = (unsigned char *)calloc(cap, 1);
  if (!t->k || !t->used) sp_oom_die();
  t->mask = cap - 1;
}
static size_t sp_iset_slot(const sp_iset *t, sp_int v) {
  size_t i = (size_t)(((uint64_t)v * 11400714819323198485ULL) >> 32) & t->mask;
  while (t->used[i] && t->k[i] != v) i = (i + 1) & t->mask;
  return i;
}
/* 1 when v was not in the set yet (it is now) */
static int sp_iset_add(sp_iset *t, sp_int v) {
  size_t i = sp_iset_slot(t, v);
  if (t->used[i]) return 0;
  t->used[i] = 1;
  t->k[i] = v;
  return 1;
}
static int sp_iset_has(const sp_iset *t, sp_int v) { return t->used[sp_iset_slot(t, v)]; }
static void sp_iset_free(sp_iset *t) { free(t->k); free(t->used); }
static void sp_iset_add_all(sp_iset *t, sp_IntArray *a) {
  for (sp_int i = 0; i < a->len; i++) sp_iset_add(t, a->data[a->start + i]);
}
/* whether a set over one side beats scanning it once per element of the other */
static int sp_setop_hashes(sp_int scanned, sp_int probes) {
  return scanned > SP_SETOP_LINEAR && probes > SP_SETOP_LINEAR;
}
void sp_IntArray_uniq_bang(sp_IntArray*a){SP_GC_ROOT(a);if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}
  if(SP_UNLIKELY(a->nilbits)){sp_oint*p=ia_pack(a);sp_int w=0;for(sp_int i=0;i<a->len;i++){int dup=0;for(sp_int j=0;j<w;j++)if(ia_oeq(p[j],p[i])){dup=1;break;}if(!dup)p[w++]=p[i];}ia_unpack(a,p,w);return;}
  sp_int *d = a->data + a->start, w = 0;
  if (a->len <= SP_SETOP_LINEAR) {
    for (sp_int i = 0; i < a->len; i++) {
      int dup = 0;
      for (sp_int j = 0; j < w && !dup; j++) dup = d[j] == d[i];
      if (!dup) d[w++] = d[i];
    }
  }
  else {
    sp_iset t; sp_iset_init(&t, a->len);
    for (sp_int i = 0; i < a->len; i++) if (sp_iset_add(&t, d[i])) d[w++] = d[i];
    sp_iset_free(&t);
  }
  a->len = w;
}
void sp_IntArray_shuffle_bang(sp_IntArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}if(SP_UNLIKELY(a->nilbits)){sp_oint*p=ia_pack(a);for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);sp_oint t=p[i];p[i]=p[j];p[j]=t;}ia_unpack(a,p,a->len);return;}for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);sp_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
sp_IntArray*sp_IntArray_shuffle(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*b=sp_IntArray_dup(a);sp_IntArray_shuffle_bang(b);return b;}
/* Array#sample: nil for `[].sample`, as CRuby. Issue #536. */
sp_oint sp_IntArray_sample_o(sp_IntArray*a){SP_GC_ROOT(a);if(!a||a->len<=0)return sp_oint_nil();return sp_IntArray_oget(a,sp_krand_below(a->len));}
/* Issue #745/#832: empty min/max answer nil; without the guard, the first
   read is uninitialized memory. A nil element beside a number has raised in
   sp_IntArray_nil_cmp_ck by now, so the nils met here are the whole array. */
sp_oint sp_IntArray_min_o(sp_IntArray*a){if(!a||a->len<=0)return sp_oint_nil();sp_int i=0;while(i<a->len&&sp_IntArray_elem_nil(a,i))i++;if(i>=a->len)return sp_oint_nil();sp_int m=a->data[a->start+i];for(;i<a->len;i++)if(a->data[a->start+i]<m)m=a->data[a->start+i];return sp_oint_of(m);}
sp_oint sp_IntArray_max_o(sp_IntArray*a){if(!a||a->len<=0)return sp_oint_nil();sp_int i=0;while(i<a->len&&sp_IntArray_elem_nil(a,i))i++;if(i>=a->len)return sp_oint_nil();sp_int m=a->data[a->start+i];for(;i<a->len;i++)if(a->data[a->start+i]>m)m=a->data[a->start+i];return sp_oint_of(m);}
sp_int sp_IntArray_sum(sp_IntArray*a,sp_int init){sp_int s=init;for(sp_int i=0;i<a->len;i++)s+=a->data[a->start+i];return s;}
/* include / index / rindex / delete look for a NUMBER: a nil element never
   matches one (its slot's 0 is not the value 0) */
sp_bool sp_IntArray_include(sp_IntArray*a,sp_int v){if(!a)return FALSE;for(sp_int i=0;i<a->len;i++)if(a->data[a->start+i]==v&&!sp_IntArray_elem_nil(a,i))return TRUE;return FALSE;}
sp_int sp_IntArray_index(sp_IntArray*a,sp_int v){for(sp_int i=0;i<a->len;i++)if(a->data[a->start+i]==v&&!sp_IntArray_elem_nil(a,i))return i;return -1;}
sp_int sp_IntArray_rindex(sp_IntArray*a,sp_int v){for(sp_int i=a->len-1;i>=0;i--)if(a->data[a->start+i]==v&&!sp_IntArray_elem_nil(a,i))return i;return -1;}
sp_oint sp_IntArray_delete_at_o(sp_IntArray*a,sp_int i){SP_GC_ROOT(a);if(!a)return sp_oint_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return sp_oint_nil();}if(i<0)i+=a->len;if(i<0||i>=a->len)return sp_oint_nil();sp_oint v=sp_IntArray_oget(a,i);for(sp_int j=i;j<a->len-1;j++)a->data[a->start+j]=a->data[a->start+j+1];if(SP_UNLIKELY(a->nilbits)){sp_nilbit_clr(a->nilbits,a->start+i);SP_NILBITS_MOVE_A(a, a->cap, a->start+i+1, a->start+i, a->len-i-1);}a->len--;return v;}
sp_oint sp_IntArray_delete_o(sp_IntArray*a,sp_int v){SP_GC_ROOT(a);if(!a)return sp_oint_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return sp_oint_nil();}sp_int w=0;if(SP_UNLIKELY(a->nilbits)){sp_oint*p=ia_pack(a);for(sp_int i=0;i<a->len;i++)if(p[i].nil||p[i].v!=v)p[w++]=p[i];sp_int d=a->len-w;ia_unpack(a,p,w);return d>0?sp_oint_of(v):sp_oint_nil();}for(sp_int i=0;i<a->len;i++){if(a->data[a->start+i]!=v){a->data[a->start+w]=a->data[a->start+i];w++;}}sp_int d=a->len-w;a->len=w;return d>0?sp_oint_of(v):sp_oint_nil();}  /* CRuby: nil when absent */
sp_bool sp_IntArray_delete_nil(sp_IntArray*a){SP_GC_ROOT(a);if(!a||!a->nilbits)return FALSE;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return FALSE;}sp_oint*p=ia_pack(a);sp_int w=0;for(sp_int i=0;i<a->len;i++)if(!p[i].nil)p[w++]=p[i];sp_bool ch=w!=a->len;ia_unpack(a,p,w);sp_IntArray_drop_nilbits(a);return ch;}
/* Issue #788: clamp i so a very-negative index doesn't underflow past
   a->start and write into the array's GC header. */
void sp_IntArray_insert(sp_IntArray*a,sp_int i,sp_int v){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len+1;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)(-(a->len+1))));while(i>a->len)sp_IntArray_push_nil(a);/* CRuby pads with nils past the end */sp_IntArray_push(a,0);for(sp_int j=a->len-1;j>i;j--)a->data[a->start+j]=a->data[a->start+j-1];if(SP_UNLIKELY(a->nilbits))SP_NILBITS_MOVE_A(a, a->cap, a->start+i, a->start+i+1, a->len-1-i);a->data[a->start+i]=v;}
void sp_IntArray_insert_nil(sp_IntArray*a,sp_int i){SP_GC_ROOT(a);if(!a)return;sp_int j=i<0?i+a->len+1:i;sp_IntArray_insert(a,i,0);if(a->frozen||j<0)return;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->start+j);}
/* The set operations below hash past SP_SETOP_LINEAR elements; an array
   carrying nil bits takes the element-wise sp_oint scan instead (nil is not
   a value the sp_iset can hold apart from 0). */
sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*b=sp_IntArray_new();SP_GC_ROOT(b);
  if (SP_UNLIKELY(SP_MAY_NIL(a))) {for(sp_int i=0;i<a->len;i++){sp_oint o=sp_IntArray_oget(a,i);if(!ia_include_o(b,o))sp_IntArray_push_o(b,o);}return b;}
  if (a->len > SP_SETOP_LINEAR) {
    sp_iset t; sp_iset_init(&t, a->len);
    for (sp_int i = 0; i < a->len; i++) { sp_int v = a->data[a->start + i]; if (sp_iset_add(&t, v)) sp_IntArray_push(b, v); }
    sp_iset_free(&t);
  }
  else for(sp_int i=0;i<a->len;i++){int found=0;for(sp_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}
  return b;}
sp_IntArray*sp_IntArray_intersect(sp_IntArray*a,sp_IntArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_IntArray*r=sp_IntArray_new();SP_GC_ROOT(r);if(!a||!b)return r;
  if (SP_UNLIKELY(SP_MAY_NIL(a) || SP_MAY_NIL(b))) {for(sp_int i=0;i<a->len;i++){sp_oint v=sp_IntArray_oget(a,i);if(ia_include_o(b,v)&&!ia_include_o(r,v))sp_IntArray_push_o(r,v);}return r;}
  if (sp_setop_hashes(b->len, a->len)) {
    sp_iset in_b, seen; sp_iset_init(&in_b, b->len); sp_iset_init(&seen, a->len);
    sp_iset_add_all(&in_b, b);
    for (sp_int i = 0; i < a->len; i++) { sp_int v = a->data[a->start + i]; if (sp_iset_has(&in_b, v) && sp_iset_add(&seen, v)) sp_IntArray_push(r, v); }
    sp_iset_free(&in_b); sp_iset_free(&seen);
  }
  else for(sp_int i=0;i<a->len;i++){sp_int v=a->data[a->start+i];if(sp_IntArray_include(b,v)&&!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}
  return r;}
sp_bool sp_IntArray_intersect_p(sp_IntArray*a,sp_IntArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return 0;
  if (SP_UNLIKELY(SP_MAY_NIL(a) || SP_MAY_NIL(b))) {for(sp_int i=0;i<a->len;i++)if(ia_include_o(b,sp_IntArray_oget(a,i)))return 1;return 0;}
  if (sp_setop_hashes(b->len, a->len)) {
    sp_iset in_b; sp_iset_init(&in_b, b->len); sp_iset_add_all(&in_b, b);
    sp_bool any = 0;
    for (sp_int i = 0; i < a->len && !any; i++) any = sp_iset_has(&in_b, a->data[a->start + i]);
    sp_iset_free(&in_b);
    return any;
  }
  for(sp_int i=0;i<a->len;i++)if(sp_IntArray_include(b,a->data[a->start+i]))return 1;return 0;}
sp_IntArray*sp_IntArray_union(sp_IntArray*a,sp_IntArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_IntArray*r=sp_IntArray_new();SP_GC_ROOT(r);
  if (SP_UNLIKELY((a && SP_MAY_NIL(a)) || (b && SP_MAY_NIL(b)))) {if(a)for(sp_int i=0;i<a->len;i++){sp_oint v=sp_IntArray_oget(a,i);if(!ia_include_o(r,v))sp_IntArray_push_o(r,v);}if(b){for(sp_int i=0;i<b->len;i++){sp_oint v=sp_IntArray_oget(b,i);if(!ia_include_o(r,v))sp_IntArray_push_o(r,v);}}return r;}
  sp_int n = (a ? a->len : 0) + (b ? b->len : 0);
  if (n > SP_SETOP_LINEAR) {
    sp_iset seen; sp_iset_init(&seen, n);
    if (a) for (sp_int i = 0; i < a->len; i++) { sp_int v = a->data[a->start + i]; if (sp_iset_add(&seen, v)) sp_IntArray_push(r, v); }
    if (b) for (sp_int i = 0; i < b->len; i++) { sp_int v = b->data[b->start + i]; if (sp_iset_add(&seen, v)) sp_IntArray_push(r, v); }
    sp_iset_free(&seen);
  }
  else{if(a)for(sp_int i=0;i<a->len;i++){sp_int v=a->data[a->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}if(b){for(sp_int i=0;i<b->len;i++){sp_int v=b->data[b->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}}}
  return r;}
/* Array#- / Array#difference: keep every LHS element NOT in RHS,
   preserving the LHS's duplicates. `[1,1,2,3] - [3]` is `[1,1,2]`. */
sp_IntArray*sp_IntArray_difference(sp_IntArray*a,sp_IntArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_IntArray*r=sp_IntArray_new();SP_GC_ROOT(r);if(!a)return r;
  if (SP_UNLIKELY(SP_MAY_NIL(a) || (b && SP_MAY_NIL(b)))) {for(sp_int i=0;i<a->len;i++){sp_oint v=sp_IntArray_oget(a,i);if(!ia_include_o(b,v))sp_IntArray_push_o(r,v);}return r;}
  if (b && sp_setop_hashes(b->len, a->len)) {
    sp_iset in_b; sp_iset_init(&in_b, b->len); sp_iset_add_all(&in_b, b);
    for (sp_int i = 0; i < a->len; i++) { sp_int v = a->data[a->start + i]; if (!sp_iset_has(&in_b, v)) sp_IntArray_push(r, v); }
    sp_iset_free(&in_b);
  }
  else for(sp_int i=0;i<a->len;i++){sp_int v=a->data[a->start+i];if(!sp_IntArray_include(b,v))sp_IntArray_push(r,v);}
  return r;}
void sp_IntArray_unshift(sp_IntArray*a,sp_int v){SP_GC_ROOT(a);if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY);return;}if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}
else{sp_int e=a->len+1;if(e>a->cap)sp_IntArray_set_cap(a,a->cap*2+1);memmove(a->data+1,a->data,sizeof(sp_int)*a->len);if(SP_UNLIKELY(a->nilbits))SP_NILBITS_MOVE_A(a, a->cap, 0, 1, a->len);a->data[0]=v;a->len++;}}
void sp_IntArray_unshift_nil(sp_IntArray*a){SP_GC_ROOT(a);if(!a)return;sp_IntArray_unshift(a,0);if(a->frozen)return;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, a->start);}
/* A nil element joins as nil.to_s, the empty string, the way the Array's
   inspect renders it as nil. */
const char*sp_IntArray_join(sp_IntArray*a,const char*sep){if(!sep)sep="";size_t sl=sp_str_byte_len(sep),cap=256;char*buf=(char*)sp_pl_alloc(cap);size_t len=0;for(sp_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}char tmp[32];sp_int v=a->data[a->start+i];int n=sp_IntArray_elem_nil(a,i)?0:snprintf(tmp,32,"%lld",(long long)v);if(len+n>=cap){cap*=2;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,tmp,n);len+=n;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);sp_str_set_len(r,len);sp_pl_free(buf);return r;}
sp_bool sp_IntArray_eq(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){if(a->data[a->start+i]!=b->data[b->start+i])return FALSE;if(SP_UNLIKELY(a->nilbits||b->nilbits)&&sp_IntArray_elem_nil(a,i)!=sp_IntArray_elem_nil(b,i))return FALSE;}return TRUE;}
/* Array#<=> for IntArray. Lexicographic: per-element compare, shorter
   array sorts before longer if all common elements match
   (`[1,2] <=> [1,2,3] == -1`). NULL recv compares equal to NULL, lower
   than any non-NULL. */
sp_oint sp_IntArray_cmp_o(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return sp_oint_of(a==b?0:(a?1:-1));sp_int n=a->len<b->len?a->len:b->len;for(sp_int i=0;i<n;i++){sp_oint av=sp_IntArray_oget(a,i),bv=sp_IntArray_oget(b,i);if(av.nil||bv.nil){if(av.nil&&bv.nil)continue;return sp_oint_nil();}if(av.v<bv.v)return sp_oint_of(-1);if(av.v>bv.v)return sp_oint_of(1);}if(a->len<b->len)return sp_oint_of(-1);if(a->len>b->len)return sp_oint_of(1);return sp_oint_of(0);}

/* ============================ sp_FloatArray ============================ */
static void fa_bits_copy(sp_FloatArray*d,sp_int dpos,sp_FloatArray*s,sp_int spos,sp_int n){if(!s||!s->nilbits||!sp_nilbits_any(s->nilbits,spos,spos+n))return;if(!d->nilbits)SP_NILBITS_NEW_A(d);for(sp_int i=0;i<n;i++)if(sp_nilbit_get(s->nilbits,spos+i))SP_NILBIT_SET_A(d, dpos+i);}
static void fa_unpack(sp_FloatArray*a,sp_ofloat*p,sp_int n){if(a->nilbits)sp_nilbits_clear_range(a->nilbits,0,a->len);for(sp_int i=0;i<n;i++){a->data[i]=p[i].v;if(p[i].nil){if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, i);}}a->len=n;sp_pl_free(p);}
static int fa_feq(sp_float x,sp_float v){return x==v||(v!=v&&x!=x&&memcmp(&x,&v,sizeof v)==0);}   /* == with the NaN identity fallback (#3650) */
static int fa_oeq(sp_ofloat x,sp_ofloat y){return x.nil?y.nil:(!y.nil&&fa_feq(x.v,y.v));}
static sp_bool fa_include_o(sp_FloatArray*a,sp_ofloat o){return o.nil?sp_FloatArray_has_nil(a):sp_FloatArray_include(a,o.v);}
void sp_FloatArray_unshift(sp_FloatArray*a,sp_float v){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}sp_FloatArray_push(a,0.0);if(a->len>1)memmove(&a->data[1],&a->data[0],(size_t)(a->len-1)*sizeof(sp_float));if(SP_UNLIKELY(a->nilbits))SP_NILBITS_MOVE_A(a, a->cap, 0, 1, a->len-1);a->data[0]=v;}
void sp_FloatArray_unshift_nil(sp_FloatArray*a){SP_GC_ROOT(a);if(!a)return;sp_FloatArray_unshift(a,0.0);if(a->frozen)return;if(!a->nilbits)SP_NILBITS_NEW_A(a);SP_NILBIT_SET_A(a, 0);}
/* CRuby's ruby_float_step_size: how many values beg.step(end, unit) yields,
   floored with an epsilon so float drift never drops or adds one. NaN
   anywhere answers NaN, which a `i < n` walk treats as none. */
sp_float sp_float_step_size(sp_float beg,sp_float end,sp_float unit,sp_int excl){
  if(isinf(unit))return unit>0?beg<=end:beg>=end;
  sp_float n=(end-beg)/unit;
  sp_float err=(fabs(beg)+fabs(end)+fabs(end-beg))/fabs(unit)*DBL_EPSILON;
  if(err>0.5)err=0.5;
  if(excl){
    if(n<=0)return 0;
    n=(n<1)?0:floor(n-err);
    sp_float d=((n+1)*unit)+beg;
    if(beg<end){if(d<end)n++;}
    else if(beg>end){if(d>end)n++;}
  }
  else{
    if(n<0)return 0;
    n=floor(n+err);
    sp_float d=((n+1)*unit)+beg;
    if(beg<end){if(d<=end)n++;}
    else if(beg>end){if(d>=end)n++;}
  }
  return n+1;
}
/* (beg..end).step(step) / Float#step materialised as a FloatArray, following
   CRuby's ruby_float_step value for value. step==0 raises ArgumentError like
   CRuby. */
sp_FloatArray*sp_FloatArray_from_step(sp_float beg,sp_float end,sp_float step,sp_int excl){
  if(step==0.0)sp_raise_cls("ArgumentError","step can't be 0");
  /* CRuby rejects a NaN range bound at construction with this message. */
  if(isnan(beg)||isnan(end))sp_raise_cls("ArgumentError","bad value for range");
  sp_float n=sp_float_step_size(beg,end,step,excl);
  /* An infinite end (or an enormous span) drives n to infinity or past what can
     be materialised; CRuby loops forever there. Raise rather than exhaust
     memory. The 1<<30 cap matches sp_IntArray_from_range's sanity bound. */
  if(n>=(sp_float)(1LL<<30))sp_raise_cls("RangeError","range too large to materialize");
  sp_FloatArray*a=sp_FloatArray_new();
  for(sp_int i=0;i<n;i++)sp_FloatArray_push(a,sp_float_step_at(beg,end,step,i));
  return a;
}
sp_ofloat sp_FloatArray_min_o(sp_FloatArray*a){if(!a||a->len==0)return sp_ofloat_nil();  /* CRuby: nil on empty (#4288) */ sp_int i=0;while(i<a->len&&sp_FloatArray_elem_nil(a,i))i++;if(i>=a->len)return sp_ofloat_nil();sp_float m=a->data[i];for(;i<a->len;i++)if(a->data[i]<m)m=a->data[i];return sp_ofloat_of(m);}
sp_ofloat sp_FloatArray_max_o(sp_FloatArray*a){if(!a||a->len==0)return sp_ofloat_nil();  /* CRuby: nil on empty (#4288) */ sp_int i=0;while(i<a->len&&sp_FloatArray_elem_nil(a,i))i++;if(i>=a->len)return sp_ofloat_nil();sp_float m=a->data[i];for(;i<a->len;i++)if(a->data[i]>m)m=a->data[i];return sp_ofloat_of(m);}
/* Array#sum for floats: Kahan-Babuska-Neumaier compensated summation,
   matching CRuby (float addition isn't associative, so a naive fold of
   [0.1,0.2,0.3] yields 0.6000000000000001 instead of 0.6). The step, with
   CRuby's NaN/Infinity arms, is sp_float_sum_step in sp_array.h -- shared with
   the boxed fold so the two cannot drift. */
sp_float sp_FloatArray_sum(sp_FloatArray*a,sp_float init){sp_float s=init,c=0.0;for(sp_int i=0;i<a->len;i++)sp_float_sum_step(&s,&c,a->data[i]);return a->len?s+c:init;}
void sp_FloatArray_replace(sp_FloatArray*dst,sp_FloatArray*src){if(dst==src)return;if(dst->frozen){sp_raise_frozen_array_at(dst,SP_BUILTIN_FLT_ARRAY);return;}dst->len=0;sp_FloatArray_drop_nilbits(dst);if(src->len>dst->cap)sp_FloatArray_set_cap(dst,src->len);memcpy(dst->data,src->data,sizeof(sp_float)*src->len);dst->len=src->len;fa_bits_copy(dst,0,src,0,src->len);}
/* a[start, len] / a[start..end] for FloatArray. Same negative-start and
   length-clamping semantics as sp_IntArray_slice. */
sp_FloatArray*sp_FloatArray_slice(sp_FloatArray*a,sp_int start,sp_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_FloatArray*b=sp_FloatArray_new();if(start>=a->len||len<=0)return b;if(len>a->len-start)len=a->len-start;if(len>b->cap)sp_FloatArray_set_cap(b,len);memcpy(b->data,a->data+start,sizeof(sp_float)*len);b->len=len;fa_bits_copy(b,0,a,start,len);return b;}
sp_FloatArray*sp_FloatArray_slice_range(sp_FloatArray*a,sp_int start,sp_int end_,sp_int excl){SP_GC_ROOT(a);if(end_<0)end_+=a->len;if(start<0)start+=a->len;if(start<0||start>a->len)return NULL;/* a start outside [-len, len] is nil (#4524); start == len is [] */sp_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_FloatArray_slice(a,start,n);}
void sp_FloatArray_reverse_bang(sp_FloatArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}if(SP_UNLIKELY(a->nilbits)){sp_ofloat*p=fa_pack(a);for(sp_int i=0,j=a->len-1;i<j;i++,j--){sp_ofloat t=p[i];p[i]=p[j];p[j]=t;}fa_unpack(a,p,a->len);return;}for(sp_int i=0,j=a->len-1;i<j;i++,j--){sp_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_FloatArray_rotate_bang(sp_FloatArray*a,sp_int n){SP_GC_ROOT(a);
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}
  if(a->len<=0)return;
  n=((n%a->len)+a->len)%a->len;
  if(n==0)return;
  if(SP_UNLIKELY(a->nilbits)){sp_ofloat*p=fa_pack(a);sp_ofloat*q=(sp_ofloat*)sp_pl_alloc(sizeof(sp_ofloat)*(size_t)a->len);if(!q)sp_oom_die();for(sp_int i=0;i<a->len;i++)q[i]=p[(i+n)%a->len];sp_pl_free(p);fa_unpack(a,q,a->len);return;}
  sp_float*d=a->data;
  sp_int rest=a->len-n;
  sp_int keep=n<rest?n:rest;
  sp_float stackbuf[32];
  sp_float*t=keep<=32?stackbuf:(sp_float*)sp_pl_alloc(sizeof(sp_float)*(size_t)keep);
  if(!t)sp_oom_die();
  if(n<=rest){
    memcpy(t,d,sizeof(sp_float)*(size_t)n);
    memmove(d,d+n,sizeof(sp_float)*(size_t)rest);
    memcpy(d+rest,t,sizeof(sp_float)*(size_t)n);
  }
  else{
    memcpy(t,d+n,sizeof(sp_float)*(size_t)rest);
    memmove(d+rest,d,sizeof(sp_float)*(size_t)n);
    memcpy(d,t,sizeof(sp_float)*(size_t)rest);
  }
  if(t!=stackbuf)sp_pl_free(t);
}
static int _sp_float_cmp(const void*a,const void*b){sp_float va=*(const sp_float*)a,vb=*(const sp_float*)b;return(va>vb)-(va<vb);}
static int fa_sort_nils(sp_FloatArray*a){if(!a->nilbits)return 0;sp_FloatArray_nil_cmp_ck(a);return sp_FloatArray_has_nil(a);}
void sp_FloatArray_sort_bang(sp_FloatArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}if(fa_sort_nils(a))return;qsort(a->data,a->len,sizeof(sp_float),_sp_float_cmp);}
void sp_FloatArray_shuffle_bang(sp_FloatArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return;}if(SP_UNLIKELY(a->nilbits)){sp_ofloat*p=fa_pack(a);for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);sp_ofloat t=p[i];p[i]=p[j];p[j]=t;}fa_unpack(a,p,a->len);return;}for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);sp_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_FloatArray*sp_FloatArray_dup(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*b=sp_FloatArray_new();sp_FloatArray_replace(b,a);return b;}
sp_FloatArray*sp_FloatArray_sort(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*b=sp_FloatArray_dup(a);sp_FloatArray_sort_bang(b);return b;}
sp_FloatArray*sp_FloatArray_shuffle(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*r=sp_FloatArray_new();sp_FloatArray_replace(r,a);sp_FloatArray_shuffle_bang(r);return r;}
sp_ofloat sp_FloatArray_sample_o(sp_FloatArray*a){SP_GC_ROOT(a);if(!a||a->len<=0)return sp_ofloat_nil();return sp_FloatArray_oget(a,sp_krand_below(a->len));}
/* IEEE 754 == on sp_float: NaN never matches; +0.0 == -0.0 (diverges from Float#eql?). */
/* a NaN is not == to itself, but CRuby's identity fallback still finds the
   very same NaN in a container (#3650) */
sp_bool sp_FloatArray_include(sp_FloatArray*a,sp_float v){if(!a)return FALSE;for(sp_int i=0;i<a->len;i++){if(fa_feq(a->data[i],v)&&!sp_FloatArray_elem_nil(a,i))return TRUE;}return FALSE;}
/* index/rindex/delete match the way sp_FloatArray_include just above does:
   == first, then the bit-pattern fallback so the Float::NAN constant finds
   itself (a computed NaN with the same bits over-matches identically).
   delete answers the deleted ELEMENT, last match, as CRuby does -- -0.0
   shows the difference -- and nil (an e never reassigned) when absent. */
sp_int sp_FloatArray_index(sp_FloatArray*a,sp_float v){if(!a)return -1;for(sp_int i=0;i<a->len;i++){if(fa_feq(a->data[i],v)&&!sp_FloatArray_elem_nil(a,i))return i;}return -1;}
sp_int sp_FloatArray_rindex(sp_FloatArray*a,sp_float v){if(!a)return -1;for(sp_int i=a->len-1;i>=0;i--){if(fa_feq(a->data[i],v)&&!sp_FloatArray_elem_nil(a,i))return i;}return -1;}
sp_ofloat sp_FloatArray_delete_o(sp_FloatArray*a,sp_float v){SP_GC_ROOT(a);if(!a)return sp_ofloat_nil();if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return sp_ofloat_nil();}sp_int w=0;sp_ofloat e=sp_ofloat_nil();if(SP_UNLIKELY(a->nilbits)){sp_ofloat*p=fa_pack(a);for(sp_int i=0;i<a->len;i++){if(!p[i].nil&&fa_feq(p[i].v,v))e=p[i];else p[w++]=p[i];}fa_unpack(a,p,w);return e;}for(sp_int i=0;i<a->len;i++){sp_float x=a->data[i];if(fa_feq(x,v))e=sp_ofloat_of(x);else{a->data[w]=x;w++;}}a->len=w;return e;}
sp_bool sp_FloatArray_delete_nil(sp_FloatArray*a){SP_GC_ROOT(a);if(!a||!a->nilbits)return FALSE;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY);return FALSE;}sp_ofloat*p=fa_pack(a);sp_int w=0;for(sp_int i=0;i<a->len;i++)if(!p[i].nil)p[w++]=p[i];sp_bool ch=w!=a->len;fa_unpack(a,p,w);sp_FloatArray_drop_nilbits(a);return ch;}
sp_FloatArray*sp_FloatArray_intersect(sp_FloatArray*a,sp_FloatArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_FloatArray*r=sp_FloatArray_new();SP_GC_ROOT(r);if(!a||!b)return r;for(sp_int i=0;i<a->len;i++){sp_ofloat v=sp_FloatArray_oget(a,i);if(fa_include_o(b,v)&&!fa_include_o(r,v))sp_FloatArray_push_o(r,v);}return r;}
sp_bool sp_FloatArray_intersect_p(sp_FloatArray*a,sp_FloatArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return 0;for(sp_int i=0;i<a->len;i++)if(fa_include_o(b,sp_FloatArray_oget(a,i)))return 1;return 0;}
sp_FloatArray*sp_FloatArray_union(sp_FloatArray*a,sp_FloatArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_FloatArray*r=sp_FloatArray_new();SP_GC_ROOT(r);if(a)for(sp_int i=0;i<a->len;i++){sp_ofloat v=sp_FloatArray_oget(a,i);if(!fa_include_o(r,v))sp_FloatArray_push_o(r,v);}if(b){for(sp_int i=0;i<b->len;i++){sp_ofloat v=sp_FloatArray_oget(b,i);if(!fa_include_o(r,v))sp_FloatArray_push_o(r,v);}}return r;}
sp_FloatArray*sp_FloatArray_difference(sp_FloatArray*a,sp_FloatArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_FloatArray*r=sp_FloatArray_new();SP_GC_ROOT(r);if(!a)return r;for(sp_int i=0;i<a->len;i++){sp_ofloat v=sp_FloatArray_oget(a,i);if(!fa_include_o(b,v))sp_FloatArray_push_o(r,v);}return r;}
/* insert(i, v) on a Float array, as sp_IntArray_insert does on an Integer
   one: a negative index counts from past the end, and the gap before an index
   past the end is nil. */
void sp_FloatArray_insert(sp_FloatArray *a, sp_int i, sp_float v) {
  SP_GC_ROOT(a);
  if (!a) return;
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY); return; }
  sp_int orig = i;
  if (i < 0) i += a->len + 1;
  if (i < 0)
    sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",
                                          (long long)orig, (long long)(-(a->len + 1))));
  while (i > a->len) sp_FloatArray_push_nil(a);
  sp_FloatArray_splice(a, i, 0, &v, 1);
}
void sp_FloatArray_insert_nil(sp_FloatArray *a, sp_int i) {
  SP_GC_ROOT(a);
  if (!a) return;
  sp_int j = i < 0 ? i + a->len + 1 : i;
  sp_FloatArray_insert(a, i, 0.0);
  if (a->frozen || j < 0) return;
  if (!a->nilbits) SP_NILBITS_NEW_A(a);
  SP_NILBIT_SET_A(a, j);
}
/* uniq! in place, matching elements the way sp_FloatArray_uniq does */
void sp_FloatArray_uniq_bang(sp_FloatArray *a) {
  SP_GC_ROOT(a);
  if (!a) return;
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY); return; }
  if (SP_UNLIKELY(a->nilbits)) {
    sp_ofloat *p = fa_pack(a); sp_int w = 0;
    for (sp_int i = 0; i < a->len; i++) { int dup = 0; for (sp_int j = 0; j < w && !dup; j++) dup = fa_oeq(p[j], p[i]); if (!dup) p[w++] = p[i]; }
    fa_unpack(a, p, w);
    return;
  }
  sp_int w = 0;
  for (sp_int i = 0; i < a->len; i++) {
    sp_float v = a->data[i];
    int dup = 0;
    for (sp_int j = 0; j < w && !dup; j++)
      dup = fa_feq(a->data[j], v);
    if (!dup) a->data[w++] = v;
  }
  a->len = w;
}
sp_FloatArray*sp_FloatArray_uniq(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*b=sp_FloatArray_new();SP_GC_ROOT(b);if(!a)return b;for(sp_int i=0;i<a->len;i++){sp_ofloat v=sp_FloatArray_oget(a,i);if(!fa_include_o(b,v))sp_FloatArray_push_o(b,v);}return b;}

/* ============================= sp_PtrArray ============================ */
/* `Array#delete_at(i)` -- remove and return the element at index i.
   Negative indices count from the end; NULL when out of range. */
void*sp_PtrArray_delete_at(sp_PtrArray*a,sp_int i){ sp_gc_wb((void*)a);if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;void*v=a->data[i];for(sp_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
void sp_PtrArray_reverse_bang(sp_PtrArray*a){ sp_gc_wb((void*)a);for(sp_int i=0,j=a->len-1;i<j;i++,j--){void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_PtrArray_rotate_bang(sp_PtrArray*a,sp_int n){
  if(!a)return;
  if(a->len<=0)return;
  n=((n%a->len)+a->len)%a->len;
  if(n==0)return;
  void**d=a->data;
  sp_int rest=a->len-n;
  sp_int keep=n<rest?n:rest;
  void* stackbuf[32];
  void**t=keep<=32?stackbuf:(void**)sp_pl_alloc(sizeof(void*)*(size_t)keep);
  if(!t)sp_oom_die();
  if(n<=rest){
    memcpy(t,d,sizeof(void*)*(size_t)n);
    memmove(d,d+n,sizeof(void*)*(size_t)rest);
    memcpy(d+rest,t,sizeof(void*)*(size_t)n);
  }
  else{
    memcpy(t,d+n,sizeof(void*)*(size_t)rest);
    memmove(d+rest,d,sizeof(void*)*(size_t)n);
    memcpy(d,t,sizeof(void*)*(size_t)rest);
  }
  if(t!=stackbuf)sp_pl_free(t);
}
sp_PtrArray*sp_PtrArray_dup(sp_PtrArray*a){SP_GC_ROOT(a);sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);for(sp_int i=0;i<a->len;i++)sp_PtrArray_push(b,a->data[i]);return b;}
sp_PtrArray*sp_PtrArray_slice(sp_PtrArray*a,sp_int start,sp_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);if(start>=a->len||len<=0)return b;if(len>a->len-start)len=a->len-start;for(sp_int i=0;i<len;i++)sp_PtrArray_push(b,a->data[start+i]);return b;}
void sp_PtrArray_shuffle_bang(sp_PtrArray*a){SP_GC_ROOT(a); sp_gc_wb((void*)a);for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_PtrArray*sp_PtrArray_shuffle(sp_PtrArray*a){SP_GC_ROOT(a);sp_PtrArray*b=sp_PtrArray_dup(a);sp_PtrArray_shuffle_bang(b);return b;}
void *sp_PtrArray_sample(sp_PtrArray*a){SP_GC_ROOT(a);if(a->len<=0)return NULL;return a->data[sp_krand_below(a->len)];}

/* ============================= sp_StrArray ============================ */
void sp_StrArray_replace(sp_StrArray*dst,sp_StrArray*src){if(dst==src)return;if(dst->frozen){sp_raise_frozen_array_at(dst,SP_BUILTIN_STR_ARRAY);return;} sp_gc_wb((void*)dst);dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));void*nd;if(dst->data==dst->inline_data){nd=sp_pl_alloc(sizeof(const char*)*src->len);if(!nd){perror("malloc");exit(1);}}
else{sp_gc_bytes_sub(sizeof(const char*)*dst->cap);h->size-=sizeof(const char*)*dst->cap;nd=sp_pl_realloc(dst->data,sizeof(const char*)*src->len);if(!nd){perror("realloc");exit(1);}}dst->data=(const char**)nd;dst->cap=src->len;h->size+=sizeof(const char*)*dst->cap;sp_gc_bytes_add(sizeof(const char*)*dst->cap);}memcpy(dst->data,src->data,sizeof(const char*)*src->len);dst->len=src->len;}
const char*sp_StrArray_pop(sp_StrArray*a){SP_GC_ROOT(a);if(!a||a->len<=0)return NULL;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return NULL;}return a->data[--a->len];}
const char*sp_StrArray_shift(sp_StrArray*a){SP_GC_ROOT(a);if(!a||a->len<=0)return NULL;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return NULL;}const char*v=a->data[0];memmove(a->data,a->data+1,(size_t)(--a->len)*sizeof(const char*));return v;}
/* a[start, len] / a[start..end] for StrArray. Same negative-start and
   length-clamping semantics as sp_IntArray_slice. */
sp_StrArray*sp_StrArray_slice(sp_StrArray*a,sp_int start,sp_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_StrArray*b=sp_StrArray_new();if(start>=a->len||len<=0)return b;if(len>a->len-start)len=a->len-start;for(sp_int i=0;i<len;i++)sp_StrArray_push(b,a->data[start+i]);return b;}
sp_StrArray*sp_StrArray_slice_range(sp_StrArray*a,sp_int start,sp_int end_,sp_int excl){SP_GC_ROOT(a);if(end_<0)end_+=a->len;if(start<0)start+=a->len;if(start<0||start>a->len)return NULL;/* a start outside [-len, len] is nil (#4524); start == len is [] */sp_int n=end_-start+(excl?0:1);if(n<0)n=0;return sp_StrArray_slice(a,start,n);}
void sp_StrArray_reverse_bang(sp_StrArray*a){SP_GC_ROOT(a); sp_gc_wb((void*)a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}for(sp_int i=0,j=a->len-1;i<j;i++,j--){const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_StrArray_rotate_bang(sp_StrArray*a,sp_int n){SP_GC_ROOT(a);
  if(!a)return;
  if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}
  if(a->len<=0)return;
  n=((n%a->len)+a->len)%a->len;
  if(n==0)return;
  const char**d=a->data;
  sp_int rest=a->len-n;
  sp_int keep=n<rest?n:rest;
  const char* stackbuf[32];
  const char**t=keep<=32?stackbuf:(const char**)sp_pl_alloc(sizeof(const char*)*(size_t)keep);
  if(!t)sp_oom_die();
  if(n<=rest){
    memcpy(t,d,sizeof(const char*)*(size_t)n);
    memmove(d,d+n,sizeof(const char*)*(size_t)rest);
    memcpy(d+rest,t,sizeof(const char*)*(size_t)n);
  }
  else{
    memcpy(t,d+n,sizeof(const char*)*(size_t)rest);
    memmove(d+rest,d,sizeof(const char*)*(size_t)n);
    memcpy(d,t,sizeof(const char*)*(size_t)rest);
  }
  if(t!=stackbuf)sp_pl_free(t);
}
static int _sp_str_cmp(const void*a,const void*b){return sp_str_cmp_bytes(*(const char*const*)a,*(const char*const*)b);}
void sp_StrArray_sort_bang(sp_StrArray*a){SP_GC_ROOT(a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}qsort(a->data,a->len,sizeof(const char*),_sp_str_cmp);}
/* The String side of the set operations' table (see sp_iset). `incl` picks
   the equality the operation always used: include?'s String equality
   (sp_str_eq: &, |, -, intersect?), or uniq's byte comparison. Under both,
   nil equals only nil, and equal Strings have equal bytes, so they meet in
   one hash chain. */
typedef struct { const char **k; unsigned char *used; size_t mask; int incl; } sp_sset;
static void sp_sset_init(sp_sset *t, sp_int n, int incl) {
  size_t cap = 32;
  while (cap < (size_t)n * 2) cap <<= 1;
  t->k = (const char **)malloc(cap * sizeof(const char *));
  t->used = (unsigned char *)calloc(cap, 1);
  if (!t->k || !t->used) sp_oom_die();
  t->mask = cap - 1;
  t->incl = incl;
}
static int sp_sset_eq(const sp_sset *t, const char *x, const char *y) {
  if (t->incl) return sp_str_eq(x, y);
  return x == y || (x && y && !sp_str_cmp_bytes(x, y));
}
static size_t sp_sset_slot(const sp_sset *t, const char *v) {
  uint64_t h = sp_str_hash(v ? v : sp_str_empty);
  size_t i = (size_t)(h ^ (h >> 29)) & t->mask;
  while (t->used[i] && !sp_sset_eq(t, t->k[i], v)) i = (i + 1) & t->mask;
  return i;
}
static int sp_sset_add(sp_sset *t, const char *v) {
  size_t i = sp_sset_slot(t, v);
  if (t->used[i]) return 0;
  t->used[i] = 1;
  t->k[i] = v;
  return 1;
}
static int sp_sset_has(const sp_sset *t, const char *v) { return t->used[sp_sset_slot(t, v)]; }
static void sp_sset_free(sp_sset *t) { free(t->k); free(t->used); }
static void sp_sset_add_all(sp_sset *t, sp_StrArray *a) {
  for (sp_int i = 0; i < a->len; i++) sp_sset_add(t, a->data[i]);
}
void sp_StrArray_uniq_bang(sp_StrArray*a){SP_GC_ROOT(a); sp_gc_wb((void*)a);if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}
  const char **d = a->data;
  sp_int w = 0;
  if (a->len <= SP_SETOP_LINEAR) {
    for (sp_int i = 0; i < a->len; i++) {
      int dup = 0;
      for (sp_int j = 0; j < w && !dup; j++) dup = d[j] == d[i] || (d[j] && d[i] && !sp_str_cmp_bytes(d[j], d[i]));
      if (!dup) d[w++] = d[i];
    }
  }
  else {
    sp_sset t; sp_sset_init(&t, a->len, 0);
    for (sp_int i = 0; i < a->len; i++) if (sp_sset_add(&t, d[i])) d[w++] = d[i];
    sp_sset_free(&t);
  }
  a->len = w;
}
sp_StrArray*sp_StrArray_uniq(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*b=sp_StrArray_new();if(!a)return b;
  if (a->len > SP_SETOP_LINEAR) {
    sp_sset t; sp_sset_init(&t, a->len, 0);
    for (sp_int i = 0; i < a->len; i++) if (sp_sset_add(&t, a->data[i])) sp_StrArray_push(b, a->data[i]);
    sp_sset_free(&t);
    return b;
  }
  for(sp_int i=0;i<a->len;i++){int found=0;for(sp_int j=0;j<b->len;j++){if(b->data[j]==a->data[i]||(b->data[j]&&a->data[i]&&!sp_str_cmp_bytes(b->data[j],a->data[i]))){found=1;break;}}if(!found)sp_StrArray_push(b,a->data[i]);}return b;}
/* byte_len for both the separator and each element: an embedded NUL is a byte
   of the string, and strlen stopped at it (the opportunistic NUL policy). */
const char*sp_StrArray_join(sp_StrArray*a,const char*sep){if(!sep)sep="";if(!a)return sp_str_empty;size_t sl=sp_str_byte_len(sep),cap=256;char*buf=(char*)sp_pl_alloc(cap);size_t len=0;for(sp_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}const char*_e=a->data[i]?a->data[i]:"";size_t el=sp_str_byte_len(_e);if(len+el>=cap){cap=((len+el)*2)+1;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,_e,el);len+=el;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);sp_str_set_len(r,len);sp_pl_free(buf);/* the joined encoding, part by part as CRuby picks it (sp_str_enc_step) */{int st=-1;size_t off=0;int sb=sp_str_is_binary(sep);for(sp_int i=0;i<a->len;i++){if(i>0){st=sp_str_enc_step(st,r,off,sep,sl,sb);off+=sl;}const char*_e=a->data[i]?a->data[i]:"";size_t el=sp_str_byte_len(_e);st=sp_str_enc_step(st,r,off,_e,el,a->data[i]&&sp_str_is_binary(_e));off+=el;}if(st==1)sp_str_mark_binary(r);}return r;}
sp_bool sp_StrArray_include(sp_StrArray*a,const char*v){SP_GC_ROOT(a);SP_GC_ROOT_STR(v);if(!a)return FALSE;for(sp_int i=0;i<a->len;i++)if(sp_str_eq(a->data[i],v))return TRUE;return FALSE;}
sp_StrArray*sp_StrArray_intersect(sp_StrArray*a,sp_StrArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrArray*r=sp_StrArray_new();if(!a||!b)return r;
  if (sp_setop_hashes(b->len, a->len)) {
    sp_sset in_b, seen; sp_sset_init(&in_b, b->len, 1); sp_sset_init(&seen, a->len, 1);
    sp_sset_add_all(&in_b, b);
    for (sp_int i = 0; i < a->len; i++) { const char *v = a->data[i]; if (sp_sset_has(&in_b, v) && sp_sset_add(&seen, v)) sp_StrArray_push(r, v); }
    sp_sset_free(&in_b); sp_sset_free(&seen);
    return r;
  }
  for(sp_int i=0;i<a->len;i++){const char*v=a->data[i];if(sp_StrArray_include(b,v)&&!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}return r;}
sp_bool sp_StrArray_intersect_p(sp_StrArray*a,sp_StrArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return 0;
  if (sp_setop_hashes(b->len, a->len)) {
    sp_sset in_b; sp_sset_init(&in_b, b->len, 1); sp_sset_add_all(&in_b, b);
    sp_bool any = 0;
    for (sp_int i = 0; i < a->len && !any; i++) any = sp_sset_has(&in_b, a->data[i]);
    sp_sset_free(&in_b);
    return any;
  }
  for(sp_int i=0;i<a->len;i++)if(sp_StrArray_include(b,a->data[i]))return 1;return 0;}
sp_StrArray*sp_StrArray_union(sp_StrArray*a,sp_StrArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrArray*r=sp_StrArray_new();
  sp_int n = (a ? a->len : 0) + (b ? b->len : 0);
  if (n > SP_SETOP_LINEAR) {
    sp_sset seen; sp_sset_init(&seen, n, 1);
    if (a) for (sp_int i = 0; i < a->len; i++) if (sp_sset_add(&seen, a->data[i])) sp_StrArray_push(r, a->data[i]);
    if (b) for (sp_int i = 0; i < b->len; i++) if (sp_sset_add(&seen, b->data[i])) sp_StrArray_push(r, b->data[i]);
    sp_sset_free(&seen);
    return r;
  }
  if(a)for(sp_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}if(b){for(sp_int i=0;i<b->len;i++){const char*v=b->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}}return r;}
sp_StrArray*sp_StrArray_difference(sp_StrArray*a,sp_StrArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrArray*r=sp_StrArray_new();
  if (b && sp_setop_hashes(b->len, a->len)) {
    sp_sset in_b; sp_sset_init(&in_b, b->len, 1); sp_sset_add_all(&in_b, b);
    for (sp_int i = 0; i < a->len; i++) if (!sp_sset_has(&in_b, a->data[i])) sp_StrArray_push(r, a->data[i]);
    sp_sset_free(&in_b);
    return r;
  }
  for(sp_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(b,v))sp_StrArray_push(r,v);}return r;}
/* min/max by String#<=> (byte comparison via strcmp); NULL (nil) when empty.
   nil (NULL) elements are skipped so a holey/sparse array can't crash strcmp. */
const char*sp_StrArray_min(sp_StrArray*a){if(!a||a->len<=0)return NULL;const char*m=NULL;for(sp_int i=0;i<a->len;i++){const char*x=a->data[i];if(x&&(!m||sp_str_cmp_bytes(x,m)<0))m=x;}return m;}
const char*sp_StrArray_max(sp_StrArray*a){if(!a||a->len<=0)return NULL;const char*m=NULL;for(sp_int i=0;i<a->len;i++){const char*x=a->data[i];if(x&&(!m||sp_str_cmp_bytes(x,m)>0))m=x;}return m;}
sp_int sp_StrArray_index(sp_StrArray*a,const char*v){SP_GC_ROOT(a);SP_GC_ROOT_STR(v);for(sp_int i=0;i<a->len;i++)if(sp_str_eq(a->data[i],v))return i;return -1;}
sp_int sp_StrArray_rindex(sp_StrArray*a,const char*v){SP_GC_ROOT(a);SP_GC_ROOT_STR(v);for(sp_int i=a->len-1;i>=0;i--)if(sp_str_eq(a->data[i],v))return i;return -1;}
sp_StrArray*sp_StrArray_compact(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*r=sp_StrArray_new();if(!a)return r;for(sp_int i=0;i<a->len;i++)if(a->data[i]!=NULL)sp_StrArray_push(r,a->data[i]);return r;}
/* A scalar array cannot nest, but it can hold nil, so compact has to filter. */
sp_IntArray*sp_IntArray_compact(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*r=sp_IntArray_new();if(!a)return r;for(sp_int i=0;i<a->len;i++)if(!sp_IntArray_elem_nil(a,i))sp_IntArray_push(r,a->data[a->start+i]);return r;}
sp_FloatArray*sp_FloatArray_compact(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*r=sp_FloatArray_new();if(!a)return r;for(sp_int i=0;i<a->len;i++)if(!sp_FloatArray_elem_nil(a,i))sp_FloatArray_push(r,a->data[i]);return r;}
/* compact! on one of those: the nils go in place, and the answer is whether
   any did (CRuby's self, or nil for a no-op). */
sp_bool sp_IntArray_compact_bang(sp_IntArray*a){SP_GC_ROOT(a);if(!a)return FALSE;if(a->frozen){sp_raise_frozen_array_at(a,SP_BUILTIN_INT_ARRAY);return FALSE;}return sp_IntArray_delete_nil(a);}
sp_bool sp_FloatArray_compact_bang(sp_FloatArray*a){SP_GC_ROOT(a);if(!a)return FALSE;if(a->frozen){sp_raise_frozen_array_at(a,SP_BUILTIN_FLT_ARRAY);return FALSE;}return sp_FloatArray_delete_nil(a);}
/* min, max, minmax and sort compare the elements pairwise, and nil compares
   with nothing but nil, so CRuby raises once one meets a number; an array of
   nils alone sorts and answers nil. The message names the pair the way a
   left-to-right scan meets it, which is Array#min's and #max's order (sort
   and the counted min(n)/max(n) may meet the pair the other way round).
   Reached through the nil_*_if_flagged wrappers in sp_array.h for an array
   that ever held a nil; each answers the array it was handed. */
sp_IntArray*sp_IntArray_nil_cmp_ck(sp_IntArray*a){if(!a||!a->nilbits)return a;sp_int ni=-1,vi=-1;for(sp_int i=0;i<a->len&&(ni<0||vi<0);i++){if(sp_IntArray_elem_nil(a,i)){if(ni<0)ni=i;}else if(vi<0)vi=i;}if(ni<0||vi<0)return a;if(ni<vi)sp_raise_cls("ArgumentError",sp_sprintf("comparison of NilClass with %lld failed",(long long)a->data[a->start+vi]));sp_raise_cls("ArgumentError","comparison of Integer with nil failed");return a;}
sp_FloatArray*sp_FloatArray_nil_cmp_ck(sp_FloatArray*a){if(!a||!a->nilbits)return a;sp_int ni=-1,vi=-1;for(sp_int i=0;i<a->len&&(ni<0||vi<0);i++){if(sp_FloatArray_elem_nil(a,i)){if(ni<0)ni=i;}else if(vi<0)vi=i;}if(ni<0||vi<0)return a;if(ni<vi)sp_raise_cls("ArgumentError",sp_sprintf("comparison of NilClass with %s failed",sp_float_to_s(a->data[vi])));sp_raise_cls("ArgumentError","comparison of Float with nil failed");return a;}
/* sum adds each element to the running total, and a nil is no addend: the
   TypeError names the total's class when the nil arrives -- the seed's before
   the first element (`float_seed`: a Float one), then the array's own, or
   Float once a Float seed has made it one. */
sp_IntArray*sp_IntArray_nil_sum_ck(sp_IntArray*a,int float_seed){if(!a||!a->nilbits)return a;for(sp_int i=0;i<a->len;i++)if(sp_IntArray_elem_nil(a,i))sp_raise_cls("TypeError",float_seed?"nil can't be coerced into Float":"nil can't be coerced into Integer");return a;}
sp_FloatArray*sp_FloatArray_nil_sum_ck(sp_FloatArray*a,int float_seed){if(!a||!a->nilbits)return a;for(sp_int i=0;i<a->len;i++)if(sp_FloatArray_elem_nil(a,i))sp_raise_cls("TypeError",(i>0||float_seed)?"nil can't be coerced into Float":"nil can't be coerced into Integer");return a;}
const char*sp_StrArray_delete_at(sp_StrArray*a,sp_int i){SP_GC_ROOT(a); sp_gc_wb((void*)a);if(!a)return NULL;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return NULL;}if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;const char*v=a->data[i];for(sp_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
const char*sp_StrArray_delete(sp_StrArray*a,const char*v){SP_GC_ROOT(a);SP_GC_ROOT_STR(v); sp_gc_wb((void*)a);if(!a)return NULL;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return NULL;}sp_int w=0;const char*found=NULL;for(sp_int i=0;i<a->len;i++){if(!sp_str_eq(a->data[i],v)){a->data[w]=a->data[i];w++;}
else{found=a->data[i];}}a->len=w;return found;}
void sp_StrArray_insert(sp_StrArray*a,sp_int i,const char*v){SP_GC_ROOT(a);SP_GC_ROOT_STR(v); sp_gc_wb((void*)a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}sp_int orig=i;if(i<0)i+=a->len+1;if(i<0)sp_raise_cls("IndexError",sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)(-(a->len+1))));while(i>a->len)sp_StrArray_push(a,NULL);/* CRuby pads with nils past the end */sp_StrArray_push(a,sp_str_empty);for(sp_int j=a->len-1;j>i;j--)a->data[j]=a->data[j-1];a->data[i]=v;}
void sp_StrArray_shuffle_bang(sp_StrArray*a){SP_GC_ROOT(a); sp_gc_wb((void*)a);if(!a)return;if(a->frozen){sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY);return;}for(sp_int i=a->len-1;i>0;i--){sp_int j=sp_krand_below(i+1);const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_StrArray*sp_StrArray_dup(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);return r;}
sp_StrArray*sp_StrArray_sort(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*b=sp_StrArray_dup(a);sp_StrArray_sort_bang(b);return b;}
sp_StrArray*sp_StrArray_shuffle(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);sp_StrArray_shuffle_bang(r);return r;}
const char *sp_StrArray_sample(sp_StrArray*a){SP_GC_ROOT(a);if(a->len<=0)return NULL;return a->data[sp_krand_below(a->len)];}

/* ============ poly/inspect-dependent array ops (display, concat, to_poly) ============ */
/* The members String#upto yields, in CRuby's rb_str_upto_each order of
   cases, each a fresh copy (the frozen begin is not handed out) passed to
   fn until it answers nonzero. */
void sp_str_upto_each(const char *s, const char *e, sp_int excl, int (*fn)(const char *, void *), void *arg) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(e);
  if (!s || !e) return;
  size_t sl = sp_str_byte_len(s), el = sp_str_byte_len(e);
  int ascii = 1;
  for (size_t i = 0; i < sl; i++) if ((unsigned char)s[i] >= 0x80) ascii = 0;
  for (size_t i = 0; i < el; i++) if ((unsigned char)e[i] >= 0x80) ascii = 0;
  /* one ASCII character at each end: every byte between, so ("A".."c")
     holds the punctuation between "Z" and "a" */
  if (ascii && sl == 1 && el == 1) {
    int lim = (unsigned char)e[0] + (excl ? 0 : 1);
    for (int ch = (unsigned char)s[0]; ch < lim; ch++) {
      char one = (char)ch;
      if (fn(sp_str_from_bytes(&one, 1), arg)) return;
    }
    return;
  }
  /* two all-digit ends: the numbers between, zero-padded to the begin's
     width, so ("9".."11") holds "9", "10", "11" (#3549) and ("1".."010")
     stops at "10". Past 18 digits the succ walk below serves. */
  int digits = ascii && sl > 0 && el > 0 && sl <= 18 && el <= 18;
  for (size_t i = 0; digits && i < sl; i++) if (s[i] < '0' || s[i] > '9') digits = 0;
  for (size_t i = 0; digits && i < el; i++) if (e[i] < '0' || e[i] > '9') digits = 0;
  if (digits) {
    long long bi = strtoll(s, NULL, 10), ei = strtoll(e, NULL, 10);
    for (; excl ? bi < ei : bi <= ei; bi++) {
      char buf[32];
      int n = snprintf(buf, sizeof buf, "%.*lld", (int)sl, bi);
      if (fn(sp_str_from_bytes(buf, (size_t)n), arg)) return;
    }
    return;
  }
  /* otherwise String#succ from the begin up to the end, never past the
     end's length, so ("a".."bb") runs through "z" and on to "bb", and
     ("aa".."z") -- whose begin is the end's successor -- is empty */
  int cmp = sp_str_cmp_bytes(s, e);
  if (cmp > 0 || (excl && cmp == 0)) return;
  const char *after = sp_str_succ(e);
  SP_GC_ROOT_STR(after);
  /* `cur` walks the range via String#succ, allocating a fresh heap string each
     step; the next sp_str_alloc can trigger a GC that would sweep both the
     array under construction and the current (unrooted) succ string, so the
     strcpy read freed memory (#3152). Root the slot -- it tracks each succ
     reassignment. */
  const char *cur = s;
  SP_GC_ROOT_STR(cur);
  while (!sp_str_eq(cur, after)) {
    int last = !excl && sp_str_eq(cur, e);
    if (fn(sp_str_from_bytes(cur, sp_str_byte_len(cur)), arg)) return;
    if (last) return;
    cur = sp_str_succ(cur);
    if (excl && sp_str_eq(cur, e)) return;
    size_t cl = sp_str_byte_len(cur);
    if (cl > el || cl == 0) return;
  }
}
static int sp_str_upto_push(const char *m, void *arg) {
  sp_StrArray_push((sp_StrArray *)arg, m);
  return 0;
}
sp_StrArray *sp_StrArray_from_string_range(const char *s, const char *e, sp_int excl) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(e);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  sp_str_upto_each(s, e, excl, sp_str_upto_push, a);
  return a;
}
const char*sp_IntArray_inspect(sp_IntArray*a){SP_GC_ROOT(a);return a?sp_inspect_container(sp_box_obj(a,SP_BUILTIN_INT_ARRAY)):"nil";}
const char*sp_FloatArray_inspect(sp_FloatArray*a){SP_GC_ROOT(a);return a?sp_inspect_container(sp_box_obj(a,SP_BUILTIN_FLT_ARRAY)):"nil";}
/* a nil element joins as "", as sp_IntArray_join's does */
const char*sp_FloatArray_join(sp_FloatArray*a,const char*sep){if(!sep)sep="";SP_GC_ROOT(a);size_t sl=sp_str_byte_len(sep),cap=256;char*buf=(char*)sp_pl_alloc(cap);size_t len=0;if(a){for(sp_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}const char*es=sp_float_opt_to_s(sp_FloatArray_oget(a,i));size_t el=strlen(es);if(len+el>=cap){while(len+el>=cap)cap*=2;buf=(char*)sp_pl_realloc(buf,cap);}memcpy(buf+len,es,el);len+=el;}}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);sp_str_set_len(r,len);sp_pl_free(buf);return r;}
/* Two nils are ==; a nil beside a number is not. */
sp_bool sp_FloatArray_eq(sp_FloatArray*a,sp_FloatArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){int an=sp_FloatArray_elem_nil(a,i),bn=sp_FloatArray_elem_nil(b,i);if(an!=bn)return FALSE;if(!an&&a->data[i]!=b->data[i])return FALSE;}return TRUE;}
const char*sp_StrArray_inspect(sp_StrArray*a){SP_GC_ROOT(a);return a?sp_inspect_container(sp_box_obj(a,SP_BUILTIN_STR_ARRAY)):"nil";}
const char*sp_PtrArray_inspect(sp_PtrArray*a){if(!a)return SPL("nil");SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(sp_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,"#<Object>");}sp_String_append(s,"]");return sp_str_dup(s->data);}
/* Array#slice_before(delim): start a new chunk before each element == delim. */
sp_PtrArray*sp_IntArray_slice_before(sp_IntArray*a,sp_int d){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a)return out;sp_IntArray*cur=sp_IntArray_new();SP_GC_ROOT(cur);for(sp_int i=0;i<a->len;i++){sp_oint e=sp_IntArray_oget(a,i);if(!e.nil&&e.v==d&&cur->len>0){sp_PtrArray_push(out,cur);cur=sp_IntArray_new();}sp_IntArray_push_o(cur,e);}if(cur->len>0)sp_PtrArray_push(out,cur);return out;}
/* Array#slice_after(delim): end a chunk after each element == delim. */
sp_PtrArray*sp_IntArray_slice_after(sp_IntArray*a,sp_int d){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a)return out;sp_IntArray*cur=sp_IntArray_new();SP_GC_ROOT(cur);for(sp_int i=0;i<a->len;i++){sp_oint e=sp_IntArray_oget(a,i);sp_IntArray_push_o(cur,e);if(!e.nil&&e.v==d){sp_PtrArray_push(out,cur);cur=sp_IntArray_new();}}if(cur->len>0)sp_PtrArray_push(out,cur);return out;}
sp_PtrArray *sp_IntArray_product(sp_IntArray *a, sp_IntArray *b) {
  SP_GC_ROOT(a); SP_GC_ROOT(b);
  sp_PtrArray *out = sp_PtrArray_new();
  SP_GC_ROOT(out);
  if (!a || !b) return out;
  for (sp_int i = 0; i < a->len; i++) {
    for (sp_int j = 0; j < b->len; j++) {
      sp_IntArray *pair = sp_IntArray_new();
      sp_IntArray_push_o(pair, sp_IntArray_oget(a, i));   /* the rows carry the operands' nils */
      sp_IntArray_push_o(pair, sp_IntArray_oget(b, j));
      sp_PtrArray_push(out, pair);
    }
  }
  return out;
}
const char*sp_PtrArray_str_join(sp_PtrArray*a,const char*sep){SP_GC_ROOT(a);SP_GC_ROOT_STR(sep);sp_int al=a->len;if(al==0)return sp_str_empty;size_t sl=strlen(sep),total=0;for(sp_int i=0;i<al;i++){if(i>0)total+=sl;sp_String*s=(sp_String*)a->data[i];if(s)total+=(size_t)s->len;}char*r=sp_str_alloc(total);size_t cur=0;for(sp_int i=0;i<al;i++){if(i>0){memcpy(r+cur,sep,sl);cur+=sl;}sp_String*s=(sp_String*)a->data[i];if(s&&s->len){memcpy(r+cur,s->data,(size_t)s->len);cur+=(size_t)s->len;}}return r;}
sp_RbVal sp_IntArray_index_poly(sp_IntArray *a, sp_int v)         {SP_GC_ROOT(a); sp_int n = sp_IntArray_index(a, v);   return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_IntArray_rindex_poly(sp_IntArray *a, sp_int v)        {SP_GC_ROOT(a); sp_int n = sp_IntArray_rindex(a, v);  return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_StrArray_index_poly(sp_StrArray *a, const char *v)     {SP_GC_ROOT(a);SP_GC_ROOT_STR(v); sp_int n = sp_StrArray_index(a, v);   return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_StrArray_rindex_poly(sp_StrArray *a, const char *v)    {SP_GC_ROOT(a);SP_GC_ROOT_STR(v); sp_int n = sp_StrArray_rindex(a, v);  return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_oint sp_IntArray_index_opt(sp_IntArray *a, sp_int v)          {SP_GC_ROOT(a); sp_int n = sp_IntArray_index(a, v);   return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
sp_oint sp_IntArray_rindex_opt(sp_IntArray *a, sp_int v)         {SP_GC_ROOT(a); sp_int n = sp_IntArray_rindex(a, v);  return n < 0 ? sp_oint_nil() : sp_oint_of(n); }
/* index(nil) / rindex(nil): the first (last) nil element */
sp_oint sp_IntArray_index_nil(sp_IntArray *a, int rev)          { if (!a || !a->nilbits) return sp_oint_nil(); if (rev) { for (sp_int i = a->len - 1; i >= 0; i--) if (sp_IntArray_elem_nil(a, i)) return sp_oint_of(i); } else { for (sp_int i = 0; i < a->len; i++) if (sp_IntArray_elem_nil(a, i)) return sp_oint_of(i); } return sp_oint_nil(); }
sp_oint sp_FloatArray_index_nil(sp_FloatArray *a, int rev)      { if (!a || !a->nilbits) return sp_oint_nil(); if (rev) { for (sp_int i = a->len - 1; i >= 0; i--) if (sp_FloatArray_elem_nil(a, i)) return sp_oint_of(i); } else { for (sp_int i = 0; i < a->len; i++) if (sp_FloatArray_elem_nil(a, i)) return sp_oint_of(i); } return sp_oint_nil(); }
sp_RbVal sp_FloatArray_index_poly(sp_FloatArray *a, sp_float v)   {SP_GC_ROOT(a); sp_int n = sp_FloatArray_index(a, v);  return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_FloatArray_rindex_poly(sp_FloatArray *a, sp_float v)  {SP_GC_ROOT(a); sp_int n = sp_FloatArray_rindex(a, v); return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* A boxed needle compares only for a Float or Integer tag; every other tag
   misses, never riding a to-f coercion that would have parsed a String or
   interned a Symbol into a false hit -- and delete into a lost element.
   Bignum, Rational and Complex miss too, where CRuby's == can match: the
   family-wide trade, the Int arm fails the C build on the same shapes. A nil
   needle finds a nil element. */
sp_RbVal sp_FloatArray_index_key(sp_FloatArray *a, sp_RbVal v)  {SP_GC_ROOT(a); if (v.tag == SP_TAG_FLT) return sp_FloatArray_index_poly(a, v.v.f); if (v.tag == SP_TAG_INT) return sp_FloatArray_index_poly(a, (sp_float)v.v.i); if (v.tag == SP_TAG_NIL) return sp_box_oint(sp_FloatArray_index_nil(a, 0)); return sp_box_nil(); }
sp_RbVal sp_FloatArray_rindex_key(sp_FloatArray *a, sp_RbVal v) {SP_GC_ROOT(a); if (v.tag == SP_TAG_FLT) return sp_FloatArray_rindex_poly(a, v.v.f); if (v.tag == SP_TAG_INT) return sp_FloatArray_rindex_poly(a, (sp_float)v.v.i); if (v.tag == SP_TAG_NIL) return sp_box_oint(sp_FloatArray_index_nil(a, 1)); return sp_box_nil(); }
sp_ofloat sp_FloatArray_delete_key(sp_FloatArray *a, sp_RbVal v) {SP_GC_ROOT(a); if (v.tag == SP_TAG_FLT) return sp_FloatArray_delete_o(a, v.v.f); if (v.tag == SP_TAG_INT) return sp_FloatArray_delete_o(a, (sp_float)v.v.i); if (v.tag == SP_TAG_NIL) { sp_FloatArray_delete_nil(a); return sp_ofloat_nil(); } return sp_ofloat_nil(); }
const int64_t *sp_IntArray_ffi_data(sp_IntArray *a) { return a ? (const int64_t *)(a->data + a->start) : (const int64_t *)0; }
const double  *sp_FloatArray_ffi_data(sp_FloatArray *a) { return a ? (const double *)a->data : (const double *)0; }
sp_IntArray *sp_IntArray_concat(sp_IntArray *a, sp_IntArray *b) { SP_GC_ROOT(a); SP_GC_ROOT(b); sp_IntArray *r = sp_IntArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_IntArray_push_o(r, sp_IntArray_oget(a, i)); if (b) for (sp_int i = 0; i < b->len; i++) sp_IntArray_push_o(r, sp_IntArray_oget(b, i)); return r; }
sp_StrArray *sp_StrArray_concat(sp_StrArray *a, sp_StrArray *b) {SP_GC_ROOT(a);SP_GC_ROOT(b); sp_StrArray *r = sp_StrArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_StrArray_push(r, sp_StrArray_get(a, i)); if (b) for (sp_int i = 0; i < b->len; i++) sp_StrArray_push(r, sp_StrArray_get(b, i)); return r; }
sp_FloatArray *sp_FloatArray_concat(sp_FloatArray *a, sp_FloatArray *b) {SP_GC_ROOT(a);SP_GC_ROOT(b); sp_FloatArray *r = sp_FloatArray_new(); SP_GC_ROOT(r); if (a) for (sp_int i = 0; i < a->len; i++) sp_FloatArray_push_o(r, sp_FloatArray_oget(a, i)); if (b) for (sp_int i = 0; i < b->len; i++) sp_FloatArray_push_o(r, sp_FloatArray_oget(b, i)); return r; }
/* A typed array widened whole into a poly one boxes its nil elements as nil,
   as sp_PolyArray_from_int_array does: once boxed as a number, nothing
   downstream -- a set operation, ==, a zip row -- can tell. */
sp_PolyArray *sp_IntArray_to_poly(sp_IntArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (!a) return r;
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_IntArray_box_elem(a, i));
  return r;
}
sp_PolyArray *sp_StrArray_to_poly_fmt(sp_StrArray *a) {SP_GC_ROOT(a);
  sp_PolyArray *r = sp_PolyArray_new();
  if (!a) return r;
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_str(a->data[i]));
  return r;
}
sp_PolyArray *sp_FloatArray_to_poly(sp_FloatArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (!a) return r;
  for (sp_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_FloatArray_box_elem(a, i));
  return r;
}
sp_IntArray *sp_IntArray_slice_bang(sp_IntArray *a, sp_int from, sp_int n) {SP_GC_ROOT(a);
  if (!a) return sp_IntArray_new();
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_INT_ARRAY); return sp_IntArray_new(); }
  /* a start past the end (or before it after normalizing) is nil, not an
     empty slice: only start == len answers [] (#3607) */
  if (from < 0) from += a->len;
  if (from < 0 || from > a->len) return NULL;
  if (n < 0) return NULL;   /* slice!(start, -1) is nil, not an empty slice */
  if (from + n > a->len) n = a->len - from;
  sp_IntArray *r = sp_IntArray_new();
  SP_GC_ROOT(r);
  for (sp_int i = 0; i < n; i++) sp_IntArray_push_o(r, sp_IntArray_oget(a, from + i));
  if (from == 0) {
    if (SP_UNLIKELY(a->nilbits)) sp_nilbits_clear_range(a->nilbits, a->start, a->start + n);
    a->start += n;
    a->len -= n;
  }
else {
    for (sp_int i = from; i + n < a->len; i++) a->data[a->start + i] = a->data[a->start + i + n];
    if (SP_UNLIKELY(a->nilbits)) { SP_NILBITS_MOVE_A(a, a->cap, a->start + from + n, a->start + from, a->len - from - n); sp_nilbits_clear_range(a->nilbits, a->start + a->len - n, a->start + a->len); }
    a->len -= n;
  }
  return r;
}
sp_FloatArray *sp_FloatArray_slice_bang(sp_FloatArray *a, sp_int from, sp_int n) {SP_GC_ROOT(a);
  if (!a) return sp_FloatArray_new();
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_FLT_ARRAY); return sp_FloatArray_new(); }
  /* a start past the end (or before it after normalizing) is nil, not an
     empty slice: only start == len answers [] (#3607) */
  if (from < 0) from += a->len;
  if (from < 0 || from > a->len) return NULL;
  if (n < 0) return NULL;   /* slice!(start, -1) is nil, not an empty slice */
  if (from + n > a->len) n = a->len - from;
  sp_FloatArray *r = sp_FloatArray_new();
  SP_GC_ROOT(r);
  for (sp_int i = 0; i < n; i++) sp_FloatArray_push_o(r, sp_FloatArray_oget(a, from + i));
  for (sp_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  if (SP_UNLIKELY(a->nilbits)) { SP_NILBITS_MOVE_A(a, a->cap, from + n, from, a->len - from - n); sp_nilbits_clear_range(a->nilbits, a->len - n, a->len); }
  a->len -= n;
  return r;
}
sp_StrArray *sp_StrArray_slice_bang(sp_StrArray *a, sp_int from, sp_int n) {SP_GC_ROOT(a); sp_gc_wb((void*)a);
  if (!a) return sp_StrArray_new();
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_STR_ARRAY); return sp_StrArray_new(); }
  /* a start past the end (or before it after normalizing) is nil, not an
     empty slice: only start == len answers [] (#3607) */
  if (from < 0) from += a->len;
  if (from < 0 || from > a->len) return NULL;
  if (n < 0) return NULL;   /* slice!(start, -1) is nil, not an empty slice */
  if (from + n > a->len) n = a->len - from;
  sp_StrArray *r = sp_StrArray_new();
  for (sp_int i = 0; i < n; i++) sp_StrArray_push(r, a->data[from + i]);
  for (sp_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
sp_PtrArray *sp_PtrArray_slice_bang(sp_PtrArray *a, sp_int from, sp_int n) {SP_GC_ROOT(a); sp_gc_wb((void*)a);
  if (!a) return sp_PtrArray_new_scan(NULL);
  if (a->frozen) { sp_raise_frozen_array_at(a, SP_BUILTIN_PTR_ARRAY); return sp_PtrArray_new_scan(a->scan_elem); }
  /* a start past the end (or before it after normalizing) is nil, not an
     empty slice: only start == len answers [] (#3607) */
  if (from < 0) from += a->len;
  if (from < 0 || from > a->len) return NULL;
  if (n < 0) return NULL;   /* slice!(start, -1) is nil, not an empty slice */
  if (from + n > a->len) n = a->len - from;
  sp_PtrArray *r = sp_PtrArray_new_scan(a->scan_elem);
  for (sp_int i = 0; i < n; i++) sp_PtrArray_push(r, a->data[from + i]);
  for (sp_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
