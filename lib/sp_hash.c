/* sp_hash.c -- cold CRUD ops for the 4 non-poly-valued typed hashes (see
 * sp_hash.h). None of these types are hot in optcarrot, so the whole
 * surface -- not just materializers -- lives here rather than in
 * spinel_rt.h. sp_gc_alloc / sp_mark_string / sp_str_hash / sp_str_eq /
 * sp_inspect_container are lib-visible. */
#include "sp_hash.h"

/* D3b-ii: a nil VALUE of an Integer-valued typed hash is a bit per slot
   (sp_types.h `vnil`, NULL until the first nil value). The test is one
   pointer load and branch on paths that already probe. */
#define SP_HV_NIL(h, idx) (SP_UNLIKELY((h)->vnil != NULL) && sp_nilbit_get((h)->vnil, (idx)))
/* a plain read met a nil value: the emitter took the hash for nil-free */
SP_NORETURN SP_COLD void sp_raise_hash_nil_value(void){sp_raise_cls("TypeError","nil value read as an Integer from a hash Spinel typed with Integer values (the hash was not marked for nil values)");for(;;){}}

void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;sp_pl_free(h->keys);sp_pl_free(h->vals);sp_pl_free(h->order);sp_pl_free(h->vnil);}
void sp_StrIntHash_scan(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;for(sp_int i=0;i<h->cap;i++){if(h->keys[i])sp_mark_string(h->keys[i]);}}
/* default_nil is set for a hash with no explicit default ({} / {k=>v}), so a
   missing-key `[]` read surfaces Ruby nil (#801). Hash.new(N) sets default_v
   to N via _new_with_default. Proven-present internal reads use _get on
   present keys, so this only governs the miss path. */
void sp_StrIntHash_init_embedded(sp_StrIntHash*h){h->cap=16;h->mask=15;h->keys=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->vals=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->order=(const char**)sp_pl_alloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=0;h->default_nil=TRUE;h->vnil=NULL;}
sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,sp_StrIntHash_scan);sp_StrIntHash_init_embedded(h);return h;}
sp_StrIntHash*sp_StrIntHash_new_with_default(sp_int d){sp_StrIntHash*h=sp_StrIntHash_new();h->default_v=d;h->default_nil=FALSE;return h;}
void sp_StrIntHash_grow(sp_StrIntHash*h){SP_GC_ROOT(h); sp_gc_wb((void*)h);sp_int oc=h->cap;const char**ok=h->keys;sp_int*ov=h->vals;uint64_t*ovn=h->vnil;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->vals=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->order=(const char**)sp_pl_realloc(h->order,sizeof(const char*)*h->cap);h->vnil=ovn?sp_nilbits_new(h,h->cap):NULL;h->len=0;for(sp_int i=0;i<oc;i++){if(ok[i]){sp_int idx=(sp_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];if(ovn&&sp_nilbit_get(ovn,i))sp_nilbit_set(h->vnil,idx);h->len++;}}sp_pl_free(ok);sp_pl_free(ov);if(ovn)sp_nilbits_free(h,ovn,oc);}
/* The slot holding k, or -1 */
static sp_int sp_StrIntHash_slot(sp_StrIntHash*h,const char*k){if(!h)return -1;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return idx;idx=(idx+1)&h->mask;}return -1;}
/* A plain read: the emitter uses it where it holds the value nil-free, so a
   nil value raises here rather than reading as 0 (D3b-ii's W1 backstop) */
sp_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);if(!h)return 0;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){if(SP_HV_NIL(h,idx))sp_raise_hash_nil_value();return h->vals[idx];}idx=(idx+1)&h->mask;}return h->default_v;}
/* the entry's value with its nil; a miss is nil (no default) */
sp_oint sp_StrIntHash_vget(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);sp_int idx=sp_StrIntHash_slot(h,k);if(idx<0||SP_HV_NIL(h,idx))return sp_oint_nil();return sp_oint_of(h->vals[idx]);}
/* Issue #801: maybe-missing public `[]` read. Answers the default on a miss:
   nil for a no-default hash, the explicit default for Hash.new(N).
   Proven-present reads keep using _get. */
sp_oint sp_StrIntHash_oget(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);if(!h)return sp_oint_nil();sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return SP_HV_NIL(h,idx)?sp_oint_nil():sp_oint_of(h->vals[idx]);idx=(idx+1)&h->mask;}return h->default_nil?sp_oint_nil():sp_oint_of(h->default_v);}
void sp_StrIntHash_set(sp_StrIntHash*h,const char*k,sp_int v){SP_GC_ROOT(h);SP_GC_ROOT_STR(k); if(!k){sp_raise_cls("TypeError","no implicit conversion of nil into String");return;} sp_gc_wb((void*)h);if(h->len*2>=h->cap)sp_StrIntHash_grow(h);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;if(SP_UNLIKELY(h->vnil!=NULL))sp_nilbit_clr(h->vnil,idx);return;}idx=(idx+1)&h->mask;}k=sp_hash_key_str(k);sp_gc_wb((void*)h);h->keys[idx]=k;h->vals[idx]=v;if(SP_UNLIKELY(h->vnil!=NULL))sp_nilbit_clr(h->vnil,idx);h->order[h->len]=k;h->len++;}
/* `h[k] = nil`: the entry with its bit set */
void sp_StrIntHash_set_nil(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);sp_StrIntHash_set(h,k,0);sp_int idx=sp_StrIntHash_slot(h,k);if(idx<0)return;if(!h->vnil)h->vnil=sp_nilbits_new(h,h->cap);sp_nilbit_set(h->vnil,idx);}
sp_bool sp_StrIntHash_has_key(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
/* h.fetch(k, d) in one probe: the value, or d when k is absent (the
   hash's own default does not apply to fetch) */
sp_oint sp_StrIntHash_fetch_or(sp_StrIntHash*h,const char*k,sp_oint d){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);if(!h)return d;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return SP_HV_NIL(h,idx)?sp_oint_nil():sp_oint_of(h->vals[idx]);idx=(idx+1)&h->mask;}return d;}
/* Hash#value? -- scan values in insertion order. Issue #738. */
sp_bool sp_StrIntHash_has_value(sp_StrIntHash*h,sp_int v){if(!h)return FALSE;for(sp_int i=0;i<h->len;i++){sp_oint o=sp_StrIntHash_vget(h,h->order[i]);if(!o.nil&&o.v==v)return TRUE;}return FALSE;}
/* value?(nil) */
sp_bool sp_StrIntHash_has_nil_value(sp_StrIntHash*h){if(!h||!h->vnil)return FALSE;for(sp_int i=0;i<h->len;i++)if(sp_StrIntHash_vget(h,h->order[i]).nil)return TRUE;return FALSE;}
sp_int sp_StrIntHash_length(sp_StrIntHash*h){return h->len;}
void sp_StrIntHash_delete(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k); sp_gc_wb((void*)h);uint64_t*vn=h->vnil;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=0;if(vn)sp_nilbit_clr(vn,idx);h->len--;sp_int j=(idx+1)&h->mask;while(h->keys[j]){sp_int nj=(sp_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];if(vn){if(sp_nilbit_get(vn,j))sp_nilbit_set(vn,idx);sp_nilbit_clr(vn,j);}h->keys[j]=NULL;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(sp_str_eq(h->order[oi],k)){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
/* `h.delete(k)` as a value: the entry's value with its nil, or nil on a miss */
sp_oint sp_StrIntHash_delete_o(sp_StrIntHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);sp_oint o=sp_StrIntHash_vget(h,k);sp_StrIntHash_delete(h,k);return o;}
sp_StrArray*sp_StrIntHash_keys(sp_StrIntHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
sp_IntArray*sp_StrIntHash_values(sp_StrIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_IntArray_push_o(a,sp_StrIntHash_vget(h,h->order[i]));return a;}
sp_StrIntHash*sp_StrIntHash_merge(sp_StrIntHash*a,sp_StrIntHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrIntHash*r=sp_StrIntHash_new();SP_GC_ROOT(r);r->default_v=a->default_v;r->default_nil=a->default_nil;for(sp_int i=0;i<a->len;i++)sp_StrIntHash_oset(r,a->order[i],sp_StrIntHash_vget(a,a->order[i]));for(sp_int i=0;i<b->len;i++)sp_StrIntHash_oset(r,b->order[i],sp_StrIntHash_vget(b,b->order[i]));return r;}
void sp_StrIntHash_update(sp_StrIntHash*a,sp_StrIntHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);for(sp_int i=0;i<b->len;i++)sp_StrIntHash_oset(a,b->order[i],sp_StrIntHash_vget(b,b->order[i]));}
sp_StrIntHash*sp_StrIntHash_dup(sp_StrIntHash*h){SP_GC_ROOT(h);sp_StrIntHash*r=sp_StrIntHash_new();SP_GC_ROOT(r);r->default_v=h->default_v;r->default_nil=h->default_nil;for(sp_int i=0;i<h->len;i++)sp_StrIntHash_oset(r,h->order[i],sp_StrIntHash_vget(h,h->order[i]));return r;}
sp_StrIntHash*sp_StrIntHash_replace(sp_StrIntHash*h,sp_StrIntHash*o){SP_GC_ROOT(h);SP_GC_ROOT(o); sp_gc_wb((void*)h);if(!h)return h;if(h==o)return h;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;if(h->vnil)memset(h->vnil,0,sp_nilbits_words(h->cap)*sizeof(uint64_t));h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_StrIntHash_oset(h,o->order[i],sp_StrIntHash_vget(o,o->order[i]));return h;}
void sp_StrIntHash_clear(sp_StrIntHash*h){ sp_gc_wb((void*)h);if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;if(h->vnil)memset(h->vnil,0,sp_nilbits_words(h->cap)*sizeof(uint64_t));h->len=0;}
sp_bool sp_StrIntHash_eq(sp_StrIntHash*a,sp_StrIntHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrIntHash_has_key(b,k))return FALSE;sp_oint x=sp_StrIntHash_vget(a,k),y=sp_StrIntHash_vget(b,k);if(x.nil!=y.nil||(!x.nil&&x.v!=y.v))return FALSE;}return TRUE;}
void sp_StrStrHash_fin(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;sp_pl_free(h->keys);sp_pl_free(h->vals);sp_pl_free(h->order);}
void sp_StrStrHash_scan(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;for(sp_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_string(h->vals[i]);}}if(h->default_v)sp_mark_string(h->default_v);}
void sp_StrStrHash_init_embedded(sp_StrStrHash*h){h->cap=16;h->mask=15;h->keys=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->vals=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->order=(const char**)sp_pl_alloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=NULL;}
sp_StrStrHash*sp_StrStrHash_new(void){sp_StrStrHash*h=(sp_StrStrHash*)sp_gc_alloc(sizeof(sp_StrStrHash),sp_StrStrHash_fin,sp_StrStrHash_scan);sp_StrStrHash_init_embedded(h);return h;}
sp_StrStrHash*sp_StrStrHash_new_with_default(const char*d){SP_GC_ROOT_STR(d);sp_StrStrHash*h=sp_StrStrHash_new();h->default_v=d;return h;}
void sp_StrStrHash_grow(sp_StrStrHash*h){SP_GC_ROOT(h); sp_gc_wb((void*)h);sp_int oc=h->cap;const char**ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->vals=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->order=(const char**)sp_pl_realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(sp_int i=0;i<oc;i++){if(ok[i]){sp_int idx=(sp_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}sp_pl_free(ok);sp_pl_free(ov);}
/* Hashing and comparing String bytes allocate nothing, so a lookup needs
   no temporary GC roots, including while value? scans the table. */
const char*sp_StrStrHash_get(sp_StrStrHash*h,const char*k){if(!h)return NULL;sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
void sp_StrStrHash_set(sp_StrStrHash*h,const char*k,const char*v){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);SP_GC_ROOT_STR(v); if(!k){sp_raise_cls("TypeError","no implicit conversion of nil into String");return;} sp_gc_wb((void*)h);if(h->len*2>=h->cap)sp_StrStrHash_grow(h);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}k=sp_hash_key_str(k);sp_gc_wb((void*)h);h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
sp_bool sp_StrStrHash_has_key(sp_StrStrHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
sp_bool sp_StrStrHash_has_value(sp_StrStrHash*h,const char*v){if(!h||!v)return FALSE;for(sp_int i=0;i<h->len;i++){const char*x=sp_StrStrHash_get(h,h->order[i]);if(x&&sp_str_eq(x,v))return TRUE;}return FALSE;}
sp_int sp_StrStrHash_length(sp_StrStrHash*h){return h->len;}
void sp_StrStrHash_delete(sp_StrStrHash*h,const char*k){SP_GC_ROOT(h);SP_GC_ROOT_STR(k); sp_gc_wb((void*)h);sp_int idx=(sp_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=NULL;h->len--;sp_int j=(idx+1)&h->mask;while(h->keys[j]){sp_int nj=(sp_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(sp_str_eq(h->order[oi],k)){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
sp_StrArray*sp_StrStrHash_keys(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
sp_StrArray*sp_StrStrHash_values(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_StrStrHash_get(h,h->order[i]));return a;}
sp_StrStrHash*sp_StrStrHash_invert(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrStrHash*r=sp_StrStrHash_new();for(sp_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrStrHash_set(r,sp_StrStrHash_get(h,k),k);}return r;}
void sp_StrStrHash_update(sp_StrStrHash*a,sp_StrStrHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);for(sp_int i=0;i<b->len;i++)sp_StrStrHash_set(a,b->order[i],sp_StrStrHash_get(b,b->order[i]));}
sp_StrStrHash*sp_StrStrHash_dup(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrStrHash*r=sp_StrStrHash_new();r->default_v=h->default_v;for(sp_int i=0;i<h->len;i++)sp_StrStrHash_set(r,h->order[i],sp_StrStrHash_get(h,h->order[i]));return r;}
sp_StrStrHash*sp_StrStrHash_merge(sp_StrStrHash*a,sp_StrStrHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_StrStrHash*r=sp_StrStrHash_new();if(a){r->default_v=a->default_v;for(sp_int i=0;i<a->len;i++)sp_StrStrHash_set(r,a->order[i],sp_StrStrHash_get(a,a->order[i]));}if(b){for(sp_int i=0;i<b->len;i++)sp_StrStrHash_set(r,b->order[i],sp_StrStrHash_get(b,b->order[i]));}return r;}
sp_StrStrHash*sp_StrStrHash_replace(sp_StrStrHash*h,sp_StrStrHash*o){SP_GC_ROOT(h);SP_GC_ROOT(o); sp_gc_wb((void*)h);if(!h)return h;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_StrStrHash_set(h,o->order[i],sp_StrStrHash_get(o,o->order[i]));return h;}
void sp_StrStrHash_clear(sp_StrStrHash*h){ sp_gc_wb((void*)h);if(!h)return;for(sp_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}
sp_bool sp_StrStrHash_eq(sp_StrStrHash*a,sp_StrStrHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_StrStrHash_get(a,k),sp_StrStrHash_get(b,k)))return FALSE;}return TRUE;}
void sp_IntStrHash_fin(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;sp_pl_free(h->keys);sp_pl_free(h->vals);sp_pl_free(h->order);sp_pl_free(h->used);}
void sp_IntStrHash_scan(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;for(sp_int i=0;i<h->cap;i++)if(h->used[i])sp_mark_string(h->vals[i]);if(h->default_v)sp_mark_string(h->default_v);}
void sp_IntStrHash_init_embedded(sp_IntStrHash*h){h->cap=16;h->mask=15;h->keys=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->vals=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->order=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)h->cap);h->used=(sp_bool*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_bool));h->len=0;h->default_v=NULL;}
sp_IntStrHash*sp_IntStrHash_new(void){sp_IntStrHash*h=(sp_IntStrHash*)sp_gc_alloc(sizeof(sp_IntStrHash),sp_IntStrHash_fin,sp_IntStrHash_scan);sp_IntStrHash_init_embedded(h);return h;}
sp_IntStrHash*sp_IntStrHash_new_with_default(const char*d){SP_GC_ROOT_STR(d);sp_IntStrHash*h=sp_IntStrHash_new();h->default_v=d;return h;}
void sp_IntStrHash_grow(sp_IntStrHash*h){ sp_gc_wb((void*)h);sp_int oc=h->cap,ol=h->len;sp_int*ok=h->keys;const char**ov=h->vals;sp_bool*ou=h->used;sp_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->vals=(const char**)sp_pl_zalloc((size_t)h->cap*sizeof(const char*));h->order=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)h->cap);h->used=(sp_bool*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_bool));h->len=ol;for(sp_int i=0;i<oc;i++){if(!ou[i])continue;sp_int k=ok[i];const char*v=ov[i];sp_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(sp_int i=0;i<ol;i++)h->order[i]=oo[i];sp_pl_free(ok);sp_pl_free(ov);sp_pl_free(ou);sp_pl_free(oo);}
void sp_IntStrHash_set(sp_IntStrHash*h,sp_int k,const char*v){SP_GC_ROOT(h);SP_GC_ROOT_STR(v); sp_gc_wb((void*)h);if(h->len*2>=h->cap)sp_IntStrHash_grow(h);sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
const char*sp_IntStrHash_get(sp_IntStrHash*h,sp_int k){if(!h)return NULL;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
sp_IntStrHash*sp_IntStrHash_merge(sp_IntStrHash*a,sp_IntStrHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_IntStrHash*r=sp_IntStrHash_new();if(a){r->default_v=a->default_v;for(sp_int i=0;i<a->len;i++)sp_IntStrHash_set(r,a->order[i],sp_IntStrHash_get(a,a->order[i]));}if(b){for(sp_int i=0;i<b->len;i++)sp_IntStrHash_set(r,b->order[i],sp_IntStrHash_get(b,b->order[i]));}return r;}
void sp_IntStrHash_update(sp_IntStrHash*a,sp_IntStrHash*b){if(!a||!b||a==b)return;SP_GC_ROOT(a);SP_GC_ROOT(b);for(sp_int i=0;i<b->len;i++)sp_IntStrHash_set(a,b->order[i],sp_IntStrHash_get(b,b->order[i]));}
sp_bool sp_IntStrHash_has_key(sp_IntStrHash*h,sp_int k){sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
sp_bool sp_IntStrHash_has_value(sp_IntStrHash*h,const char*v){if(!h||!v)return FALSE;for(sp_int i=0;i<h->len;i++){const char*x=sp_IntStrHash_get(h,h->order[i]);if(x&&sp_str_eq(x,v))return TRUE;}return FALSE;}
sp_int sp_IntStrHash_length(sp_IntStrHash*h){return h->len;}
sp_IntArray*sp_IntStrHash_keys(sp_IntStrHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
sp_StrArray*sp_IntStrHash_values(sp_IntStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(sp_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_IntStrHash_get(h,h->order[i]));return a;}
sp_IntStrHash*sp_IntStrHash_dup(sp_IntStrHash*h){SP_GC_ROOT(h);sp_IntStrHash*r=sp_IntStrHash_new();r->default_v=h->default_v;for(sp_int i=0;i<h->len;i++)sp_IntStrHash_set(r,h->order[i],sp_IntStrHash_get(h,h->order[i]));return r;}
sp_IntStrHash*sp_IntStrHash_replace(sp_IntStrHash*h,sp_IntStrHash*o){SP_GC_ROOT(h);SP_GC_ROOT(o);if(!h)return h;for(sp_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_IntStrHash_set(h,o->order[i],sp_IntStrHash_get(o,o->order[i]));return h;}
sp_bool sp_IntStrHash_eq(sp_IntStrHash*a,sp_IntStrHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){sp_int k=a->order[i];if(!sp_IntStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_IntStrHash_get(a,k),sp_IntStrHash_get(b,k)))return FALSE;}return TRUE;}
/* Int → Int typed hash. Mirrors sp_IntStrHash's open-addressing
   layout (used[] bitmap so 0/-1 keys are distinguishable from
   empty), with int-valued slots (#865). */
void sp_IntIntHash_fin(void*p){sp_IntIntHash*h=(sp_IntIntHash*)p;sp_pl_free(h->keys);sp_pl_free(h->vals);sp_pl_free(h->order);sp_pl_free(h->used);sp_pl_free(h->vnil);}
/* default_nil is set for a hash with no explicit default, so a missing-key
   `[]` read surfaces Ruby nil (#801). Hash.new(N) sets default_v via
   _new_with_default. */
void sp_IntIntHash_init_embedded(sp_IntIntHash*h){h->cap=16;h->mask=15;h->keys=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->vals=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->order=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)h->cap);h->used=(sp_bool*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_bool));h->len=0;h->default_v=0;h->default_nil=TRUE;h->vnil=NULL;}
sp_IntIntHash*sp_IntIntHash_new(void){sp_IntIntHash*h=(sp_IntIntHash*)sp_gc_alloc(sizeof(sp_IntIntHash),sp_IntIntHash_fin,NULL);sp_IntIntHash_init_embedded(h);return h;}
sp_IntIntHash*sp_IntIntHash_new_with_default(sp_int d){sp_IntIntHash*h=sp_IntIntHash_new();h->default_v=d;h->default_nil=FALSE;return h;}
void sp_IntIntHash_grow(sp_IntIntHash*h){sp_int oc=h->cap,ol=h->len;sp_int*ok=h->keys;sp_int*ov=h->vals;sp_bool*ou=h->used;sp_int*oo=h->order;uint64_t*ovn=h->vnil;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->vals=(sp_int*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_int));h->order=(sp_int*)sp_pl_alloc(sizeof(sp_int)*(size_t)h->cap);h->used=(sp_bool*)sp_pl_zalloc((size_t)h->cap*sizeof(sp_bool));h->vnil=ovn?sp_nilbits_new(h,h->cap):NULL;h->len=ol;for(sp_int i=0;i<oc;i++){if(!ou[i])continue;sp_int k=ok[i];sp_int v=ov[i];sp_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;if(ovn&&sp_nilbit_get(ovn,i))sp_nilbit_set(h->vnil,di);}for(sp_int i=0;i<ol;i++)h->order[i]=oo[i];sp_pl_free(ok);sp_pl_free(ov);sp_pl_free(ou);sp_pl_free(oo);if(ovn)sp_nilbits_free(h,ovn,oc);}
void sp_IntIntHash_set(sp_IntIntHash*h,sp_int k,sp_int v){SP_GC_ROOT(h);if(h->len*2>=h->cap)sp_IntIntHash_grow(h);sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;if(SP_UNLIKELY(h->vnil!=NULL))sp_nilbit_clr(h->vnil,idx);return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;if(SP_UNLIKELY(h->vnil!=NULL))sp_nilbit_clr(h->vnil,idx);h->order[h->len++]=k;}
/* The slot holding k, or -1 */
static sp_int sp_IntIntHash_slot(sp_IntIntHash*h,sp_int k){if(!h)return -1;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return idx;idx=(idx+1)&h->mask;}return -1;}
/* `h[k] = nil`: the entry with its bit set */
void sp_IntIntHash_set_nil(sp_IntIntHash*h,sp_int k){SP_GC_ROOT(h);sp_IntIntHash_set(h,k,0);sp_int idx=sp_IntIntHash_slot(h,k);if(idx<0)return;if(!h->vnil)h->vnil=sp_nilbits_new(h,h->cap);sp_nilbit_set(h->vnil,idx);}
/* the entry's value with its nil; a miss is nil (no default) */
sp_oint sp_IntIntHash_vget(sp_IntIntHash*h,sp_int k){sp_int idx=sp_IntIntHash_slot(h,k);if(idx<0||SP_HV_NIL(h,idx))return sp_oint_nil();return sp_oint_of(h->vals[idx]);}
/* A plain read: a nil value raises (the W1 backstop, as sp_StrIntHash_get) */
sp_int sp_IntIntHash_get(sp_IntIntHash*h,sp_int k){if(!h)return 0;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){if(SP_HV_NIL(h,idx))sp_raise_hash_nil_value();return h->vals[idx];}idx=(idx+1)&h->mask;}return h->default_v;}
sp_IntIntHash*sp_IntIntHash_merge(sp_IntIntHash*a,sp_IntIntHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);sp_IntIntHash*r=sp_IntIntHash_new();SP_GC_ROOT(r);if(a){r->default_v=a->default_v;r->default_nil=a->default_nil;for(sp_int i=0;i<a->len;i++)sp_IntIntHash_oset(r,a->order[i],sp_IntIntHash_vget(a,a->order[i]));}if(b){for(sp_int i=0;i<b->len;i++)sp_IntIntHash_oset(r,b->order[i],sp_IntIntHash_vget(b,b->order[i]));}return r;}
void sp_IntIntHash_update(sp_IntIntHash*a,sp_IntIntHash*b){if(!a||!b||a==b)return;SP_GC_ROOT(a);SP_GC_ROOT(b);for(sp_int i=0;i<b->len;i++)sp_IntIntHash_oset(a,b->order[i],sp_IntIntHash_vget(b,b->order[i]));}
/* Integer-keyed hash delete: backward-shift the probe cluster so open-addressing
   lookups stay correct, then drop the key from the insertion-order array. */
void sp_IntIntHash_delete(sp_IntIntHash*h,sp_int k){if(!h)return;uint64_t*vn=h->vnil;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->used[idx]=0;if(vn)sp_nilbit_clr(vn,idx);h->len--;sp_int j=(idx+1)&h->mask;while(h->used[j]){sp_int nj=_sp_istr_idx(h->mask,h->keys[j]);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->used[idx]=1;h->used[j]=0;if(vn){if(sp_nilbit_get(vn,j))sp_nilbit_set(vn,idx);sp_nilbit_clr(vn,j);}idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(h->order[oi]==k){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
/* `h.delete(k)` as a value: the entry's value with its nil, or nil on a miss */
sp_oint sp_IntIntHash_delete_o(sp_IntIntHash*h,sp_int k){sp_oint o=sp_IntIntHash_vget(h,k);sp_IntIntHash_delete(h,k);return o;}
void sp_IntStrHash_delete(sp_IntStrHash*h,sp_int k){ sp_gc_wb((void*)h);if(!h)return;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->used[idx]=0;h->len--;sp_int j=(idx+1)&h->mask;while(h->used[j]){sp_int nj=_sp_istr_idx(h->mask,h->keys[j]);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->used[idx]=1;h->used[j]=0;idx=j;}j=(j+1)&h->mask;}{sp_int oi=0;while(oi<=h->len){if(h->order[oi]==k){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
/* Issue #801: maybe-missing public `[]` read. Answers the default on a miss
   (nil for a no-default hash; the explicit default for Hash.new(N)).
   Proven-present reads keep using _get. */
sp_oint sp_IntIntHash_oget(sp_IntIntHash*h,sp_int k){if(!h)return sp_oint_nil();sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return SP_HV_NIL(h,idx)?sp_oint_nil():sp_oint_of(h->vals[idx]);idx=(idx+1)&h->mask;}return h->default_nil?sp_oint_nil():sp_oint_of(h->default_v);}
sp_bool sp_IntIntHash_has_key(sp_IntIntHash*h,sp_int k){sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
sp_oint sp_IntIntHash_fetch_or(sp_IntIntHash*h,sp_int k,sp_oint d){if(!h)return d;sp_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return SP_HV_NIL(h,idx)?sp_oint_nil():sp_oint_of(h->vals[idx]);idx=(idx+1)&h->mask;}return d;}
sp_int sp_IntIntHash_length(sp_IntIntHash*h){return h?h->len:0;}
sp_IntArray*sp_IntIntHash_keys(sp_IntIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);if(h)for(sp_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
sp_IntArray*sp_IntIntHash_values(sp_IntIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);if(h)for(sp_int i=0;i<h->len;i++)sp_IntArray_push_o(a,sp_IntIntHash_vget(h,h->order[i]));return a;}
sp_bool sp_IntIntHash_has_value(sp_IntIntHash*h,sp_int v){if(!h)return FALSE;for(sp_int i=0;i<h->len;i++){sp_oint o=sp_IntIntHash_vget(h,h->order[i]);if(!o.nil&&o.v==v)return TRUE;}return FALSE;}
/* value?(nil) */
sp_bool sp_IntIntHash_has_nil_value(sp_IntIntHash*h){if(!h||!h->vnil)return FALSE;for(sp_int i=0;i<h->len;i++)if(sp_IntIntHash_vget(h,h->order[i]).nil)return TRUE;return FALSE;}
sp_bool sp_IntIntHash_eq(sp_IntIntHash*a,sp_IntIntHash*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++){sp_int k=a->order[i];if(!sp_IntIntHash_has_key(b,k))return FALSE;sp_oint x=sp_IntIntHash_vget(a,k),y=sp_IntIntHash_vget(b,k);if(x.nil!=y.nil||(!x.nil&&x.v!=y.v))return FALSE;}return TRUE;}
sp_IntIntHash*sp_IntIntHash_dup(sp_IntIntHash*h){SP_GC_ROOT(h);sp_IntIntHash*r=sp_IntIntHash_new();SP_GC_ROOT(r);r->default_v=h->default_v;r->default_nil=h->default_nil;for(sp_int i=0;i<h->len;i++)sp_IntIntHash_oset(r,h->order[i],sp_IntIntHash_vget(h,h->order[i]));return r;}
sp_IntIntHash*sp_IntIntHash_replace(sp_IntIntHash*h,sp_IntIntHash*o){SP_GC_ROOT(h);SP_GC_ROOT(o);if(!h)return h;if(h==o)return h;for(sp_int i=0;i<h->cap;i++)h->used[i]=0;if(h->vnil)memset(h->vnil,0,sp_nilbits_words(h->cap)*sizeof(uint64_t));h->len=0;if(o)for(sp_int i=0;i<o->len;i++)sp_IntIntHash_oset(h,o->order[i],sp_IntIntHash_vget(o,o->order[i]));return h;}
void sp_IntIntHash_clear(sp_IntIntHash*h){if(!h)return;for(sp_int i=0;i<h->cap;i++)h->used[i]=0;if(h->vnil)memset(h->vnil,0,sp_nilbits_words(h->cap)*sizeof(uint64_t));h->len=0;}
/* Issue #851: Hash#inspect for typed-hash variants beyond
   sym_int_hash. Renders Ruby's `{"k" => v, ...}` (string keys),
   `{42 => "v", ...}` (int keys), or `{:k => v, ...}` (sym keys but
   non-int value, since the bare `k: v` shorthand only applies
   when values are inspectable as one-liners -- match CRuby). */
const char*sp_StrIntHash_inspect(sp_StrIntHash*h){SP_GC_ROOT(h);return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_INT_HASH)):SPL("nil");}
/* Hash#to_proc lookup fn -- cap is the hash, args[0] the string key. */
sp_int sp_StrIntHash_proc_fn(void *cap, sp_int argc, sp_int *args) { if (argc < 1) return 0; return sp_StrIntHash_get((sp_StrIntHash *)cap, (const char *)(uintptr_t)args[0]); }
const char*sp_StrStrHash_inspect(sp_StrStrHash*h){SP_GC_ROOT(h);return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_STR_HASH)):SPL("nil");}
const char*sp_IntStrHash_inspect(sp_IntStrHash*h){SP_GC_ROOT(h);return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_INT_STR_HASH)):SPL("nil");}
const char*sp_IntIntHash_inspect(sp_IntIntHash*h){SP_GC_ROOT(h);if(!h)return SPL("nil");sp_String*s=sp_String_new("{");SP_GC_ROOT(s);if(h){for(sp_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(h->order[i]));sp_String_append(s," => ");sp_oint o=sp_IntIntHash_vget(h,h->order[i]);sp_String_append(s,o.nil?"nil":sp_int_to_s(o.v));}}sp_String_append(s,"}");return sp_str_dup(s->data);}

/* Issue #738: Hash#to_a as poly_array of [key, value] poly_array pairs. */
sp_PolyArray*sp_StrIntHash_to_a(sp_StrIntHash*h){SP_GC_ROOT(h);sp_PolyArray*r=sp_PolyArray_new();SP_GC_ROOT(r);if(!h)return r;for(sp_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();SP_GC_ROOT(p);sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_oint(sp_StrIntHash_vget(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
sp_PolyArray*sp_StrStrHash_to_a(sp_StrStrHash*h){SP_GC_ROOT(h);sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(sp_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_StrStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
sp_PolyArray*sp_IntStrHash_to_a(sp_IntStrHash*h){SP_GC_ROOT(h);sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(sp_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_int(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_IntStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}

void sp_IntStrHash_clear(sp_IntStrHash*h){if(!h)return;for(sp_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;}
