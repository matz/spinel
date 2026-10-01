#ifndef SP_ALLOC_H
#define SP_ALLOC_H
/* Shared GC-string allocation.

   The string-heap state below is `extern` -- defined once in sp_alloc.c and
   linked from libspinel_rt.a -- so the generated program's single translation
   unit and every standalone lib C file share ONE string heap. A cold runtime C file
   (marshal, pack, json, ...) can therefore allocate GC-tracked strings directly
   without #including spinel_rt.h and inheriting its per-TU `static` heap state
   (which would otherwise create a second, never-swept string heap and leak).

   The hot allocators stay `static inline` here so each including TU still
   inlines them over the shared extern state -- the same shape sp_gc_alloc
   already uses for the extern sp_gc_heap / sp_gc_bytes object heap. */
#include "sp_types.h"   /* sp_str_hdr, sp_int, sp_float */
#include "sp_gc.h"      /* sp_gc_collect, sp_oom_die, sp_gc_str_sweep_hook */
#include "sp_time.h"    /* sp_Time for sp_box_time */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>      /* snprintf for the int/float formatters below */
#include <math.h>       /* HUGE_VAL / signbit for sp_float_to_s */

const char *sp_sprintf(const char *fmt, ...) SP_PRINTF_FORMAT(1, 2);  /* defined in the generated TU */

/* Global heap lock (Phase 1, design 6.1). Under SP_THREADS one mutex serializes
   the object- and string-heap mutations -- the trigger+collect, the calloc/
   malloc, and the list link -- so N>1 workers cannot corrupt the shared heap
   lists or byte counters. It wraps the hot allocators here (sp_str_alloc, in
   this header so the generated TU's inlined copy locks too) and in sp_alloc.c
   (sp_gc_alloc). The internal collect/sweep run under the held lock and never
   re-acquire it (no allocator re-enters from a scan/finalize callback). A single
   non-recursive mutex covers both heaps since a collection from either path
   sweeps the other under the same held lock. No-op (and no pthread dependency)
   in the single-threaded build, so that path is byte-identical.
   NOTE: at N>1 this alone is not sufficient -- a worker doing pure computation
   never reaches the lock, so stop-the-world via safepoints (added with the
   workers) is still required before the collector may run. */
#ifdef SP_THREADS
#include <pthread.h>
extern pthread_mutex_t sp_heap_lock;
#define SP_HEAP_LOCK()   pthread_mutex_lock(&sp_heap_lock)
#define SP_HEAP_UNLOCK() pthread_mutex_unlock(&sp_heap_lock)
#else
#define SP_HEAP_LOCK()   ((void)0)
#define SP_HEAP_UNLOCK() ((void)0)
#endif

/* ---- shared string-heap state (defined in sp_alloc.c) ----
   Threaded build: the string heap is per-worker (one singly-linked list + live-
   byte counter per OS worker), so sp_str_alloc pushes lock-free onto its own
   worker's list -- the shared sp_heap_lock serialized every string allocation
   and made allocation-heavy parallel workloads scale NEGATIVELY. A started green
   thread is pinned to its worker (home_wid), so only that worker's M ever pushes
   onto its list, and it pumps one green thread at a time: no concurrent push.
   The collector reaches every list under stop-the-world (all workers parked).
   The single-threaded build keeps one global list and stays byte-identical. */
/* Generations. Allocation always pushes onto the YOUNG list; a string still
   live at a sweep moves to the OLD list, which ordinary (minor) sweeps then
   skip. Without this every sweep walks the entire live set, so a program
   holding a large one -- a Merkle tree of hex digests, a parsed document --
   pays that walk again on each collection no matter how little of it changed.
   Strings have no outgoing references, so there is nothing here of the write
   barrier and forwarding a generational OBJECT heap would need: the split is
   two list heads and a byte counter.
   Reclamation of old strings is deferred to a major sweep, which runs when the
   old generation has itself grown past a threshold. */
#ifdef SP_THREADS
/* sp_worker_id / sp_active_workers / SP_MAX_WORKERS: sp_types.h */
/* One cache-line-padded slot per worker. The list head is written on EVERY
   string allocation and the byte counter read on every one, so as four bare
   [SP_MAX_WORKERS] arrays the per-worker slots sat 8 bytes apart: eight
   workers shared a single line per array, and every allocation invalidated it
   for all of them. The object heap already learned this -- sp_gc_wslot_t
   exists for exactly this reason and its comment calls the padding essential
   -- and the string heap never got the same treatment, which left it the
   dominant cost of allocation-heavy parallel work. */
/* The young list is SP_STR_YSUB lists, filled round-robin. A sweep task is
   one list, so a worker that allocated most of a cycle's strings (a busy
   request handler among idle workers) has its sweep split across the parked
   workers instead of being the one slot everyone waits on: the longest slot
   was the whole parallel phase, four milliseconds of thirty workers idle. */
#define SP_STR_YSUB 4
typedef struct {
  sp_str_hdr *young[SP_STR_YSUB];   /* per-worker young list heads */
  unsigned    rr;                   /* which young list the next allocation joins */
  size_t      young_bytes;  /* live bytes on those lists */
  sp_str_hdr *old;          /* per-worker old list head (swept on a major) */
  size_t      old_bytes;
  /* The byte count at which this worker may next ASK for a collection. The
     trigger below fires on this worker's own bytes against the threshold,
     while sp_stw_collect's early-out wants the AGGREGATE over N * threshold --
     so a worker that crosses first is refused, its bytes are not reset by any
     sweep, and it asks again on the very next allocation. Each ask takes the
     global scheduler lock: measured at 7.2 MILLION asks for 941 collections,
     99.99% of them refused, which is uncontended and free at one worker and a
     futex storm at eight (#4334). Raising the mark by one threshold on every
     ask bounds it to one per threshold's worth of allocation. */
  size_t      ask_at;
  char _pad[2 * SP_CACHELINE - (SP_STR_YSUB + 1) * sizeof(sp_str_hdr *) - sizeof(unsigned) - 3 * sizeof(size_t)];
} sp_str_wslot_t;
extern sp_str_wslot_t sp_str_wslot[SP_MAX_WORKERS];
#else
extern sp_str_hdr *sp_str_heap;          /* young list head */
extern size_t sp_str_heap_bytes;         /* young string-heap bytes */
extern sp_str_hdr *sp_str_old;           /* old list head */
extern size_t sp_str_old_bytes;          /* old string-heap bytes */
#endif
extern size_t sp_str_old_threshold;      /* old bytes that trigger a major sweep */
extern size_t sp_str_old_threshold_init; /* recompute floor for the above */
extern size_t sp_str_threshold;          /* string-GC trigger (own heuristic) */
extern size_t sp_str_threshold_init;     /* recompute floor */
extern int    sp_str_stress_checked;     /* one-shot SPINEL_GC_STRESS check */
extern int    sp_gc_stress_pin;          /* stress caps the retunes at the 2048 base (#3513) */
#ifdef SP_THREADS
void sp_alloc_stress_init(void);         /* race-free one-shot stress check (pre-helpers) */
void sp_alloc_floors_from_env(void);
void sp_alloc_worker_tune(int workers); /* size the collection budget for N workers (pre-helpers) */
#endif

extern const char sp_str_empty_data[];
#define sp_str_empty (sp_str_empty_data + 1)
/* nil.to_s / true.to_s / false.to_s: frozen, as in CRuby (lib/sp_alloc.c) */
extern const char *const sp_str_frozen_empty;
extern const char *const sp_str_frozen_true;
extern const char *const sp_str_frozen_false;
/* A chilled String is what CRuby's Symbol#to_s answers: not frozen (frozen?
   is false, and a mutation goes through), but +@ copies it as it copies a
   frozen one. Spinel keeps one per symbol (sp_sym_to_s_chilled): static
   storage with a header, the 0xfb marker, whose `next` link names
   sp_str_chilled_tag. A static string is on no sweep list, so the link is
   free to carry the mark, and no marker test -- the collector's included --
   has to learn a new byte. The symbol rides ahead of the header, so a String
   handle made from one can name it (sp_String.chilled). */
extern const sp_str_hdr sp_str_chilled_tag;
typedef struct { sp_int sym; sp_str_hdr h; unsigned char m; char d[]; } sp_str_chilled_obj;
static inline int sp_str_is_chilled(const char *s) {
  return s && ((const unsigned char *)s)[-1] == 0xfb &&
         (((const sp_str_hdr *)(s - 1)) - 1)->next == &sp_str_chilled_tag;
}
static inline sp_int sp_str_chilled_sym(const char *s) {
  return ((const sp_str_chilled_obj *)(s - offsetof(sp_str_chilled_obj, d)))->sym;
}
/* a nullable string (NULL) as the empty string, the standard C spelling of
   GNU's `s ?: sp_str_empty` */
static inline const char *sp_str_or_empty(const char *s) { return s ? s : sp_str_empty; }

/* UTF-8 char-length cache. Shared (extern) so sp_str_sweep flushes the same
   table the length helpers in spinel_rt.h populate: a per-TU split would leave
   the generated TU's cache pointing at strings the archive-side sweep already
   freed. */
/* Two ways per bucket: a scan loop reading one string while measuring another
   (a CSV parser walking the input and its field buffer) collided on every
   character with a direct-mapped table and rescanned both strings each time. */
#define SP_STR_LCACHE_BITS 6
#define SP_STR_LCACHE_WAYS 2
#define SP_STR_LCACHE_SIZE ((1u << SP_STR_LCACHE_BITS) * SP_STR_LCACHE_WAYS)
struct sp_str_lcache_entry {
  const char *s;
  size_t byte_len;
  sp_int char_len;
};
/* Per-worker (SP_TLS) in the threaded build: this string-length cache is keyed
   by string pointer and written without the heap lock (sp_str_byte_len is on the
   hot path), so a shared copy would be a data race across workers -- a torn read
   yields a wrong length and sp_str_concat's memcpy overruns. Each worker keeps
   its own; it is cleared at every safepoint park (before a sweep can recycle a
   cached string's address) and by the string sweep on the collector. */
extern SP_TLS struct sp_str_lcache_entry sp_str_lcache[SP_STR_LCACHE_SIZE];
static inline unsigned sp_str_lcache_slot(const char *s) {
  uintptr_t k = (uintptr_t)s;
  return (unsigned)((k ^ (k >> 4) ^ (k >> 12)) & ((1u << SP_STR_LCACHE_BITS) - 1))
         * SP_STR_LCACHE_WAYS;
}
/* The markers that carry a real sp_str_hdr in front of the bytes; a raw C
   string (a bare literal, a getenv result) has none and answers "not binary". */
/* 0xfb = STATIC storage that nonetheless carries a header. The 1-byte
   substring tables live in a static array, so their bytes are never allocated
   and never freed; 0xff already says that, but it says it by having no header
   at all, which leaves nowhere to put the BINARY tag or a NUL-safe length.
   0xfb is 0xff plus a header: it joins the "has a header" group below and
   stays OUT of the "is a mutable heap string" group (sp_gc's sweep,
   setbyte's in-place write, append_grow), so every static-safety property of
   0xff is kept by not being added anywhere. */
static inline int sp_str_has_hdr(const char *s) {
  unsigned char m = ((const unsigned char *)s)[-1];
  return m == 0xfe || m == 0xfc || m == 0xfd || m == 0xf1 || m == 0xfb || m == 0xfa || m == 0xf8;
}
/* "Every byte of this string is below 0x80", verified once and remembered.
   A 7-bit string indexes at fixed width, so #length is its byte length and
   `s[i]` is the byte at i -- no walk, and no probe of the pointer-keyed length
   cache either, which is what the walk had already been folded into. The bit
   is a HINT: set only after a scan proved it, cleared whenever the bytes may
   change, and every reader falls back to the cache when it is off. It says
   nothing about the string's encoding -- US-ASCII and UTF-8 are compatible and
   spinel does not distinguish them (docs/limitations.md). */
#define SP_STR_SIZE_ASCII7 0x40000000u
static inline void sp_str_ascii7_clear(const char *s) {
  if (!s || !sp_str_has_hdr(s)) return;
  (((sp_str_hdr *)(s - 1)) - 1)->size &= ~SP_STR_SIZE_ASCII7;
}
/* Forget what we know about `s`: its bytes are about to change, or its buffer
   is about to be freed and the address handed to something else. */
static inline void sp_str_lcache_drop(const char *s) {
  if (!s) return;
  sp_str_ascii7_clear(s);
  unsigned h = sp_str_lcache_slot(s);
  for (unsigned w = 0; w < SP_STR_LCACHE_WAYS; w++)
    if (sp_str_lcache[h + w].s == s) sp_str_lcache[h + w].s = NULL;
}
/* Deep-return side channel (#3227): a method whose every return path yields
   a shared-mutable string publishes the sp_String* handle here as the copy
   is read out; a demand-marked call site picks it up right after the call
   (resetting it to NULL first). Never read unless the analysis marked the
   site, so ordinary callers are untouched. */
extern SP_TLS void *_sp_ret_strbuf;

/* Cold; single definitions in sp_alloc.c. sp_str_sweep is wired to the GC via a
   constructor so it runs from sp_gc_collect regardless of which TU triggered
   the collection. */
void sp_str_sweep(void);
/* The string-sweep gate, split so the scheduler can run the middle on the
   workers that own the slots (see sp_alloc.c). */
int  sp_str_sweep_begin(int *major);
void sp_str_sweep_end(int major, size_t promoted);
void sp_str_sweep_end_excluding(int major, size_t promoted, size_t young_exclude);   /* the concurrent sweep's deferred end */
#ifdef SP_THREADS
void sp_str_sweep_old_one(int wid);
void sp_str_sweep_young_list(sp_str_hdr **head, sp_str_hdr **keep, sp_str_hdr **tail, size_t *moved, size_t *held);
size_t sp_str_sweep_old_list(sp_str_hdr **head);
size_t sp_str_bytes_total(void);   /* every worker's young string bytes */
void sp_str_sweep_young_one(int wid, int sub, sp_str_hdr **keep, sp_str_hdr **tail,
                            size_t *moved, size_t *held);
void sp_str_sweep_young_done(int wid, sp_str_hdr *keep, sp_str_hdr *tail, size_t moved, size_t held,
                             size_t *promoted);
extern int sp_str_par_done;
#endif
void sp_str_lcache_clear(void);
/* Collect + retune (see sp_alloc.c). The single-threaded allocators call the
   per-heap variants directly; sp_stw_collect (threaded) runs _all under STW. */
void sp_str_collect_retune(void);
void sp_gc_collect_retune_all(void);
int  sp_gc_collection_wanted(void);
void sp_stw_collect(void);   /* lib/sp_sched.c: stop-the-world collect (threaded) */
void sp_gc_collect_request(void);   /* explicit GC.start: same barrier, forced */

/* SPINEL_ALLOC_REPORT counters (#1336): defined in sp_alloc.c. The `on` flag
   is set once by a constructor from the env var; the hot entry points guard
   their count call on it (one predictable branch when off). */
extern int sp_alloc_report_on;
void sp_alloc_report_count(void *scan, size_t bytes);
void sp_alloc_report_str(size_t bytes);
void sp_alloc_report_tag(void *scan, const char *name);

/* Binary (ASCII-8BIT) strings. spinel keeps no per-string encoding tag:
   String#length counts UTF-8 code points while the bytes stay valid UTF-8 and
   falls back to the byte count once an invalid byte shows up, which lands on
   Ruby's answer for text and for almost all binary data. "Almost" is the gap:
   a short draw of random bytes is valid UTF-8 by chance about 1.5% of the
   time, and Random.urandom(4).length then answered 3 (#3474). One bit says
   "these bytes are data, count them as bytes" for the few producers that know
   it. It rides in the top bit of the header's `size` (the allocation total,
   far below 2GB), so no string grows by a byte. */
#define SP_STR_SIZE_BINARY 0x80000000u
/* bit 30 is SP_STR_SIZE_ASCII7 (declared above, with the length cache it
   short-circuits); the allocation total keeps the low 30, so a single string
   is bounded at 1GB rather than 2. Every reader of `size` masks. */
#define SP_STR_SIZE_MASK   0x3FFFFFFFu

static inline char *sp_str_alloc(size_t len) {
  size_t total = sizeof(sp_str_hdr) + 1 + len + 1;
  sp_str_hdr *h;
  /* String-heap pressure drives its own collection (see sp_str_heap_bytes).
     Collect BEFORE the new allocation, like sp_gc_alloc, so the string being
     built isn't yet live during the sweep. Operands of the calling op (e.g. the
     arguments to sp_str_concat) must be reachable across this point -- they are
     for rooted locals; the codegen's SP_GC_ROOT discipline is what keeps them
     so. Threshold recompute mirrors sp_gc_alloc's. */
#ifdef SP_THREADS
  int wid = sp_worker_id;
  if (!sp_str_stress_checked) { sp_str_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { sp_str_threshold = 2048; sp_str_threshold_init = 2048; sp_gc_stress_pin = 1; } }
  /* Per-worker trigger: the fast path reads only this worker's own byte count,
     no shared state. Each worker fires at the full threshold, so the aggregate
     bound scales with the worker count -- keeping the stop-the-world frequency
     per worker independent of N. A shared aggregate check (threshold/N) instead
     multiplied the collection count by N and left allocation-heavy parallel
     workloads STW-bound; this keeps them flat. */
  { size_t _yb = SP_GC_CTR_GET(sp_str_wslot[wid].young_bytes);
    /* over the threshold AND past this worker's ask mark; the threshold clause
       stays first so a retune that LOWERS it still triggers at once. */
    if (_yb > sp_str_threshold && _yb >= sp_str_wslot[wid].ask_at) {
      sp_stw_collect();
      /* A collection resets young_bytes, so this lands back at one threshold;
         a refusal leaves it where it was, so the next ask is one threshold of
         allocation later rather than the next allocation. */
      sp_str_wslot[wid].ask_at = SP_GC_CTR_GET(sp_str_wslot[wid].young_bytes) + sp_str_threshold;
    } }
  h = (sp_str_hdr *)sp_slab_alloc_str(total);
  h->size = (uint32_t)total;
  h->len = (uint32_t)len;
  h->hash = 0;
  /* A slab block's generation is a bit in its chunk (lib/sp_slab.c); only
     a string too large for the slab goes on the worker's young list.
     Publish h->next before the head store so a concurrent GC.stat walk that
     observes the new head reaches a fully-linked node (only pushes touch the
     head; the sweep runs under stop-the-world). */
  if (!sp_slab_owns(h)) {
    unsigned sub = sp_str_wslot[wid].rr++ & (SP_STR_YSUB - 1);
    h->next = sp_str_wslot[wid].young[sub];
    sp_str_wslot[wid].young[sub] = h; }
  SP_GC_CTR_ADD(sp_str_wslot[wid].young_bytes, total);
#else
  SP_HEAP_LOCK();
  if (!sp_str_stress_checked) { sp_str_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { SP_GC_CTR_SET(sp_str_threshold, 2048); sp_str_threshold_init = 2048; sp_gc_stress_pin = 1; } }
  if (SP_GC_CTR_GET(sp_str_heap_bytes) > sp_str_threshold) {
    sp_str_collect_retune();         /* sp_gc_collect runs sp_str_sweep */
  }
  h = (sp_str_hdr *)sp_slab_alloc_str(total);
  h->size = (uint32_t)total;
  h->len = (uint32_t)len;
  h->hash = 0;
  if (!sp_slab_owns(h)) { h->next = sp_str_heap; sp_str_heap = h; }
  SP_GC_CTR_ADD(sp_str_heap_bytes, total);
  SP_HEAP_UNLOCK();
#endif
  /* Don't fold string-heap pressure into sp_gc_bytes: the threshold heuristic
     in sp_gc_alloc is keyed on object-heap survivors, and the str-heap sweep
     that runs alongside (sp_str_sweep, from sp_gc_collect) doesn't add surviving
     strings back into sp_gc_bytes. Folding them in over-fires the object
     heuristic (the reason they're excluded). */
  char *body = (char *)(h + 1);
  body[0] = (char)0xfe;
  body[1 + len] = 0;
  if (sp_alloc_report_on) sp_alloc_report_str(len);
  return body + 1;
}

/* Allocate a string WITHOUT giving the collector a chance to run first.
   sp_str_alloc collects before allocating, which is right for every ordinary
   build-a-string operation: its operands are rooted. It is wrong for copying a
   pointer that CANNOT be rooted -- a bare C literal has no marker byte for
   sp_mark_string to read, and an unrooted heap string would be swept out from
   under the copy. The raise path takes one or the other on every call, so it
   copies through here. The heap simply runs a little past the threshold; the
   next ordinary allocation collects. */
static inline char *sp_str_alloc_nogc(size_t len) {
  size_t total = sizeof(sp_str_hdr) + 1 + len + 1;
  sp_str_hdr *h = (sp_str_hdr *)sp_slab_alloc_str(total);
  h->size = (uint32_t)total;
  h->len = (uint32_t)len;
  h->hash = 0;
#ifdef SP_THREADS
  { int wid = sp_worker_id;
    if (!sp_slab_owns(h)) {
      unsigned sub = sp_str_wslot[wid].rr++ & (SP_STR_YSUB - 1);
      h->next = sp_str_wslot[wid].young[sub];
      sp_str_wslot[wid].young[sub] = h; }
    SP_GC_CTR_ADD(sp_str_wslot[wid].young_bytes, total); }
#else
  SP_HEAP_LOCK();
  if (!sp_slab_owns(h)) { h->next = sp_str_heap; sp_str_heap = h; }
  SP_GC_CTR_ADD(sp_str_heap_bytes, total);
  SP_HEAP_UNLOCK();
#endif
  { char *body = (char *)(h + 1);
    body[0] = (char)0xfe;
    body[1 + len] = 0;
    if (sp_alloc_report_on) sp_alloc_report_str(len);
    return body + 1; }
}

/* Copy a message onto the string heap so it can be held by a string root.
   The source is a bare literal (every raise the runtime and the generated
   code issue passes one) or an unrooted heap string; neither can be rooted
   across an allocation, so the copy runs with no collection in between. */
static inline const char *sp_msg_heapify(const char *m) {
  if (!m) return NULL;
  size_t n = strlen(m);
  char *r = sp_str_alloc_nogc(n);
  memcpy(r, m, n);
  return r;
}

/* Raw variant: the caller writes a NUL-terminated payload whose final
   length it doesn't know yet (worst-case sized transforms: dump, gsub,
   tr, ...). Leave the header length unset so sp_str_byte_len answers
   strlen until sp_str_set_len records the real (possibly NUL-embedded)
   length; a capacity left in the header would over-read on concat and
   break byte-exact equality. */
#define SP_STR_LEN_UNSET 0xFFFFFFFFu
static inline char *sp_str_alloc_raw(size_t total_with_null) {
  char *s = sp_str_alloc(total_with_null > 0 ? total_with_null - 1 : 0);
  (((sp_str_hdr *)(s - 1)) - 1)->len = SP_STR_LEN_UNSET;
  return s;
}

static inline int sp_str_is_binary(const char *s) {
  if (!s || !sp_str_has_hdr(s)) return 0;
  return (((const sp_str_hdr *)(s - 1)) - 1)->size & SP_STR_SIZE_BINARY ? 1 : 0;
}
static inline void sp_str_mark_binary(char *s) {
  if (!s || !sp_str_has_hdr(s)) return;
  (((sp_str_hdr *)(s - 1)) - 1)->size |= SP_STR_SIZE_BINARY;
}
/* Fixed-width in ONE header decode: answers the byte length through *out and
   returns 1 when indexing this string is byte indexing (BINARY, or a scan has
   proved it 7-bit). Asking is_binary, then is_ascii7, then byte_len re-read the
   marker and the header three times over -- 66 instructions where the answer is
   two loads. */
static inline int sp_str_fixed_width(const char *s, size_t *out) {
  if (!s) { *out = 0; return 0; }
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc || m == 0xfd || m == 0xf1 || m == 0xfb || m == 0xfa || m == 0xf8) {
    const sp_str_hdr *h = ((const sp_str_hdr *)(s - 1)) - 1;
    if (h->len != SP_STR_LEN_UNSET &&
        (h->size & (SP_STR_SIZE_BINARY | SP_STR_SIZE_ASCII7))) { *out = h->len; return 1; }
  }
  *out = 0;
  return 0;
}
/* Mark and hand back, so an emitter can wrap an expression in it. */
static inline const char *sp_str_as_binary(const char *s) {
  sp_str_mark_binary((char *)s);
  return s;
}
/* The inverse, for force_encoding back to the text side. */
static inline const char *sp_str_as_text(const char *s) {
  if (s && sp_str_has_hdr(s)) (((sp_str_hdr *)(s - 1)) - 1)->size &= ~SP_STR_SIZE_BINARY;
  return s;
}
static inline int sp_str_is_ascii7(const char *s) {
  if (!s || !sp_str_has_hdr(s)) return 0;
  return (((const sp_str_hdr *)(s - 1)) - 1)->size & SP_STR_SIZE_ASCII7 ? 1 : 0;
}
static inline void sp_str_mark_ascii7(const char *s) {
  if (!s || !sp_str_has_hdr(s)) return;
  (((sp_str_hdr *)(s - 1)) - 1)->size |= SP_STR_SIZE_ASCII7;
}

static inline size_t sp_str_byte_len(const char *s) {
  if (!s) return 0;
  unsigned char marker = ((const unsigned char *)s)[-1];
  /* 0xf1 (frozen heap string / frozen literal) also carries a real sp_str_hdr
     whose len is the true byte length, so an embedded NUL survives freezing
     (#2462 dedup, .freeze). */
  if (marker == 0xfe || marker == 0xfc || marker == 0xfd || marker == 0xf1 || marker == 0xfb || marker == 0xfa || marker == 0xf8) {
    uint32_t l = (((const sp_str_hdr *)(s - 1)) - 1)->len;
    if (l != SP_STR_LEN_UNSET) return l;
  }
  return strlen(s);
}

static inline void sp_str_set_len(char *s, size_t len) {
  if (!s) return;
  unsigned char marker = ((unsigned char *)s)[-1];
  if (marker == 0xfe || marker == 0xfc || marker == 0xfd || marker == 0xf1 || marker == 0xfb || marker == 0xfa || marker == 0xf8) {
    sp_str_hdr *hd = ((sp_str_hdr *)(s - 1)) - 1;
    hd->len = (uint32_t)len;
    hd->hash = 0;  /* length change implies content change: invalidate cached hash */
    hd->size &= ~SP_STR_SIZE_ASCII7;   /* ...and the verified-7-bit answer */
  }
}

/* A fresh BINARY-tagged empty string, for the zero-length results whose source
   was binary: CRuby gives `bin[0, 0]`, `bin.byteslice(0, 0)`, `bin * 0` and
   `io.read(0)` the binary encoding, and the shared empty string cannot carry
   it -- marking that would tag it for every other holder of the same pointer.
   So these allocate, and only on that path. */
static inline char *sp_str_empty_binary(void) {
  char *r = sp_str_alloc_raw(1);
  r[0] = 0;
  sp_str_set_len(r, 0);
  sp_str_mark_binary(r);
  return r;
}

/* Byte count for the binary-safe return modes (FFI `:binstr`, native binding
   `:cbinstr`). A C function returning bytes rather than a NUL-terminated
   string publishes the exact length here just before it returns; the call site
   reads it instead of calling strlen, which would truncate at an embedded NUL.
   Lives here rather than in any one provider because both sp_net (socket
   reads) and sp_crypto (raw digests) write it.

   Per-worker in the threaded build, for the same reason the buffers it
   describes are: the value is written by the provider and read by the call
   site an expression later, so two workers in the same provider would
   otherwise hand each other the wrong length. The generated TU is compiled
   with the matching -DSP_THREADS, so both sides agree on the storage class. */
extern SP_TLS int sp_ffi_bin_len;

static inline const char *sp_str_from_bytes(const char *data, size_t len) {
  char *s = sp_str_alloc(len);
  if (data) memcpy(s, data, len);
  s[len] = 0;
  return s;
}
static inline const char *sp_str_dup_external(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *r = sp_str_alloc(n);
  memcpy(r, s, n);
  return r;
}

/* Integer / Float -> decimal string. Shared here (over the string heap) so cold
   readers such as lib/sp_json.c can format numbers without spinel_rt.h. */
/* Interpolation writers: append one part into a caller-provided buffer and
   return the new tail. emit_interp sizes the buffer from static bounds
   (SP_W_INT_MAX digits per int, literal lengths) plus strlen of the
   pre-evaluated dynamic parts, so one sp_str_alloc_raw serves the whole
   string (previously: one heap string per part + an sp_sprintf pass). */
#define SP_W_INT_MAX 21  /* -9223372036854775808 */
/* Decimal digits of u, counted up front so the digits can be written in place
   at their final position -- no scratch buffer and no second copy. */
static inline int sp_u64_ndigits(uint64_t u) {
  int d = 1;
  for (;;) {
    if (u < 10) return d;
    if (u < 100) return d + 1;
    if (u < 1000) return d + 2;
    if (u < 10000) return d + 3;
    u /= 10000; d += 4;
  }
}
/* Write u's digits so the last one lands just before `end`, two at a time from
   a pair table: half the divisions of the digit-at-a-time loop. */
static inline void sp_u64_write_back(char *end, uint64_t u) {
  static const char d2[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";
  while (u >= 100) {
    unsigned r = (unsigned)(u % 100); u /= 100;
    end -= 2; end[0] = d2[2 * r]; end[1] = d2[2 * r + 1];
  }
  if (u >= 10) { end -= 2; end[0] = d2[2 * u]; end[1] = d2[2 * u + 1]; }
  else *--end = (char)('0' + u);
}
static inline char *sp_w_int(char *p, sp_int n) {
  if (n == SP_INT_NIL) return p;  /* a nil int slot interpolates as "" */
  uint64_t u;
  if (n < 0) { *p++ = '-'; u = (uint64_t)(-(n + 1)) + 1; }
  else u = (uint64_t)n;
  p += sp_u64_ndigits(u);
  sp_u64_write_back(p, u);
  return p;
}
/* NOTE: s must be a marked spinel string (heap or codegen literal) --
   sp_str_byte_len reads the marker byte at s[-1], which is out of bounds on
   a foreign C literal. emit_interp therefore uses strlen + memcpy for
   dynamic string parts instead of this helper. */
static inline char *sp_w_str(char *p, const char *s) {
  if (!s) return p;
  size_t l = sp_str_byte_len(s);
  memcpy(p, s, l);
  return p + l;
}
static inline char *sp_w_bool(char *p, sp_bool v) {
  if (v) { memcpy(p, "true", 4); return p + 4; }
  memcpy(p, "false", 5); return p + 5;
}

/* Integer#to_s. This is one of the hottest allocating calls in the runtime --
   every interpolation of a number, every inspect of an integer array, every
   `j.to_s` in a loop -- and it went through snprintf: __printf_buffer and its
   friends were a fifth of an allocating benchmark's profile. Writing the digits
   by hand also lets the string be allocated at its real length instead of a
   fixed 32, which is less string-heap pressure per call and so fewer
   collections. */
static inline const char *sp_int_to_s(sp_int n) {
  uint64_t u;
  /* negate in unsigned space: -INT64_MIN does not fit in sp_int */
  if (n < 0) u = (uint64_t)(-(n + 1)) + 1; else u = (uint64_t)n;
  size_t len = (size_t)sp_u64_ndigits(u) + (n < 0);
  /* sp_str_alloc records the exact length and terminates it, so the digits go
     straight into the result. */
  char *b = sp_str_alloc(len);
  sp_u64_write_back(b + len, u);
  if (n < 0) b[0] = '-';
  /* digits and the sign are 7-bit by construction: the length and index paths
     then answer without scanning. The header is a fresh heap one. */
  (((sp_str_hdr *)(b - 1)) - 1)->size |= SP_STR_SIZE_ASCII7;
  return b;
}
/* Float#to_s (Ruby semantics): the shortest decimal that round-trips, fixed
   point for a decimal exponent in [-4, 15], scientific otherwise; NaN, ±Infinity
   and -0.0 match CRuby's spelling. */
/* Float#to_s / #inspect: shortest round-trip decimal. Cold (display only) and
   large, so it lives out-of-line in sp_alloc.c rather than inlining into every
   TU that formats a float. */
const char *sp_float_to_s(sp_float f);

/* ---- container payloads: a hash's tables, an array's data ----
   The blocks a container keeps beside its object (bucket arrays, an
   element buffer) came from malloc, and a request on campfire made 3,600 of
   them: a keyword hash is three 128-byte tables, and glibc's arena lock and
   coalescing were a sixth of the CPU. A block up to the slab's largest
   class comes from the calling worker's slab instead, lock-free; anything
   larger, or the slab off, is malloc as before, and sp_slab_free tells the
   two apart by address. A payload freed by the program (a grow, a rehash)
   goes into the thread's batched free list like a sweep's would. */
static inline void *sp_pl_alloc(size_t n) { return sp_slab_alloc_raw(n); }
static inline void *sp_pl_zalloc(size_t n) { void *p = sp_slab_alloc_raw(n); memset(p, 0, n); return p; }
static inline void sp_pl_free(void *p) { if (p) sp_slab_free(p); }
void *sp_pl_realloc(void *p, size_t newn);   /* lib/sp_slab.c: a slab block knows its size class, a malloc block is realloc'd */

/* ---- object construction (shared so lib C files can build values) ----
   The built-in cls_id sentinels, the core sp_box_* constructors, the object
   allocator, and sp_PolyArray. Moved here from spinel_rt.h so a standalone TU
   (sp_pack.c, sp_strscan.c, ...) can allocate and box without the per-TU heap
   state. SP_BUILTIN_FOREIGN_PTR/COMPLEX/RATIONAL are in sp_gc.h. */
#define SP_BUILTIN_ARRAY_OF(tag) (-(tag) - 1)
#define SP_BUILTIN_INT_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_INT) /* -1 */
#define SP_BUILTIN_STR_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_STR) /* -2 */
#define SP_BUILTIN_FLT_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_FLT) /* -3 */
#define SP_BUILTIN_PTR_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_OBJ) /* -6 */
#define SP_BUILTIN_SYM_ARRAY SP_BUILTIN_ARRAY_OF(SP_TAG_SYM) /* -7 */
#define SP_BUILTIN_PROC (-9)
#define SP_BUILTIN_RANGE (-10)
#define SP_BUILTIN_TIME (-11)
#define SP_BUILTIN_POLY_ARRAY (-12)
#define SP_BUILTIN_STR_INT_HASH (-13)
#define SP_BUILTIN_STR_STR_HASH (-14)
#define SP_BUILTIN_INT_STR_HASH (-15)
#define SP_BUILTIN_SYM_INT_HASH (-16)
#define SP_BUILTIN_SYM_STR_HASH (-17)
#define SP_BUILTIN_STR_POLY_HASH (-18)
#define SP_BUILTIN_SYM_POLY_HASH (-19)
#define SP_BUILTIN_POLY_POLY_HASH (-20)
#define SP_BUILTIN_OBJECT        (-21)
#define SP_BUILTIN_FIBER         (-22)
#define SP_BUILTIN_IO            (-23)
#define SP_BUILTIN_METHOD        (-24)
/* -45, not the -25 this once held: sp_gc.h had already given -25 to
   SP_BUILTIN_FOREIGN_PTR, and the two blocks cannot see each other. An
   Enumerator in a poly slot therefore read as a foreign pointer, which the
   collector deliberately does NOT trace (#4158). */
#define SP_BUILTIN_ENUMERATOR    (-45)
/* Exception lived at -13, aliasing SP_BUILTIN_STR_INT_HASH, which both put
   exceptions inside the hash cls_id block [-20,-13] (breaking is_a?(Hash) /
   poly .class for exceptions) and made a str_int_hash arriving as a poly value
   misdispatch through the Exception inspect path. Give it a distinct id below
   the hash block so the two no longer collide. */
#define SP_BUILTIN_EXCEPTION     (-28)
#define SP_BUILTIN_THREAD        (-29)
#define SP_BUILTIN_QUEUE         (-30)
#define SP_BUILTIN_MUTEX         (-31)
#define SP_BUILTIN_CONDVAR       (-32)
/* Integer-keyed Integer-valued hash. It sits BELOW the contiguous hash block
   [-20,-13] (which was full), so the "is this a hash" range checks that test
   that block must also test for this id explicitly. */
#define SP_BUILTIN_INT_INT_HASH  (-34)
#define SP_BUILTIN_BASIC_OBJECT  (-37)  /* a bare BasicObject.new instance */
#define SP_BUILTIN_STRBUF        (-40)  /* boxed sp_String* handle: a shared-
                                           mutable string stored in a container
                                           (#3227 phase 3); reads deref the live
                                           buffer, identity is the handle */
#define SP_BUILTIN_DIR           (-38)  /* an open directory handle (sp_Dir *) */
#define SP_BUILTIN_TMS           (-39)  /* Process.times -> Process::Tms */
#define SP_BUILTIN_ADDRINFO      (-46)  /* Addrinfo (sp_Addrinfo *). -46, not
                                           the -40 this once held: STRBUF above
                                           had it, so a boxed Addrinfo answered
                                           String and lost every method (#4158) */
#define SP_BUILTIN_SOCKOPT       (-47)  /* Socket::Option (sp_SockOpt *). -47,
                                           not the -41 this once held: sp_gc.h
                                           had given -41 to OPENSTRUCT (#4158) */
#define SP_BUILTIN_CURRY         (-44)  /* Proc#curry accumulator (sp_Curry *):
                                           boxed so a curried proc survives a
                                           poly slot, where it read as nil (#3885) */
#define SP_BUILTIN_MATCHDATA     (-43)  /* MatchData (sp_MatchData *): boxed so
                                           a match can live in a container */
#define SP_BUILTIN_PROCESS_STATUS (-48) /* Process::Status (sp_ProcessStatus *):
                                           boxed so waitpid2's status carries the
                                           cls_id for .signaled? dispatch. -48,
                                           not the -45 first chosen: the upstream
                                           cls_id-distinctness fix moved
                                           SP_BUILTIN_ENUMERATOR to -45 (#4158),
                                           so PROCESS_STATUS had to take the next
                                           free slot. The switch in spinel_rt.h
                                           that asserts every id is distinct will
                                           flag any future collision at compile
                                           time. */
#define SP_BUILTIN_YIELDER       (-49)  /* Enumerator::Yielder: the generator's
                                          block parameter as a VALUE, for a
                                          proc inside the body that captures
                                          it. v.p is the generator's own sp_Fiber;
                                          `y << v` on it is Fiber.yield (the
                                          proc runs on that fiber). */

static inline sp_RbVal sp_box_int(sp_int v)    { sp_RbVal r; r.tag = SP_TAG_INT;  r.cls_id = 0; r.v.i = v; return r; }
/* A NULL char* IS Ruby nil throughout the string paths (the nullable-string
   invariant); preserve that across the boxing boundary, or a boxed nil-string
   carries SP_TAG_STR and fails tag-keyed comparisons -- `defined?(x).should
   == nil` compared STR(NULL) against NIL and answered false. */
static inline sp_RbVal sp_box_str(const char *v){ sp_RbVal r; if (!v) { r.tag = SP_TAG_NIL; r.cls_id = 0; r.v.s = NULL; return r; } r.tag = SP_TAG_STR;  r.cls_id = 0; r.v.s = v; return r; }
/* A String mutator's value from a poly arm, which answers the box or the
   unboxed String by mutator: boxed either way (a NULL String is nil). */
static inline sp_RbVal sp_box_same(sp_RbVal v){ return v; }
#define SP_BOX_STR_OR_POLY(x) _Generic((x), sp_RbVal: sp_box_same, default: sp_box_str)(x)
static inline sp_RbVal sp_box_float(sp_float v){ sp_RbVal r; r.tag = SP_TAG_FLT;  r.cls_id = 0; r.v.f = v; return r; }
/* Write the full union word, not just the narrow `b` member: hash keys and
   poly equality compare bool values through `v.i`, so bytes left
   uninitialized here make two `true`s unequal (a garbage-dependent hash). */
static inline sp_RbVal sp_box_bool(sp_bool v)  { sp_RbVal r; r.tag = SP_TAG_BOOL; r.cls_id = 0; r.v.i = (v != 0); return r; }
static inline sp_RbVal sp_box_nil(void)         { sp_RbVal r; r.tag = SP_TAG_NIL;  r.cls_id = 0; r.v.i = 0; return r; }
static inline sp_RbVal sp_box_obj(void *p, int cls_id) { sp_RbVal r; r.tag = SP_TAG_OBJ; r.cls_id = cls_id; r.v.p = p; return r; }
static inline sp_RbVal sp_box_sym(sp_sym v)     { if (v == (sp_sym)-1) { sp_RbVal n; n.tag = SP_TAG_NIL; n.cls_id = 0; n.v.i = 0; return n; } sp_RbVal r; r.tag = SP_TAG_SYM;  r.cls_id = 0; r.v.i = (sp_int)v; return r; }  /* (sp_sym)-1 is the nilable-symbol sentinel: box it as nil, never as :"" */
static inline sp_RbVal sp_box_poly_array(void *p) { return sp_box_obj(p, SP_BUILTIN_POLY_ARRAY); }

/* The inverse of sp_box_int_or_nil / the float sentinel: unbox a poly into a
   flat int / float slot that may hold nil. Reading `.v.i` straight off a
   nil-tagged value yields the payload underneath the tag -- 0, an ordinary
   Integer -- so nil silently becomes 0 (or 0.0). These map the nil tag to the
   slot's reserved sentinel instead, which is what every nil? / to_s / boxing
   site on a nullable int or float already tests for. */
static inline sp_int sp_poly_as_int_or_nil(sp_RbVal v) {
  return v.tag == SP_TAG_NIL ? SP_INT_NIL : v.v.i;
}
static inline sp_float sp_poly_as_float_or_nil(sp_RbVal v) {
  return v.tag == SP_TAG_NIL ? sp_float_nil() : v.v.f;
}

/* GC object allocator. The threshold/stress state is extern (defined in
   sp_alloc.c) so every TU shares it -- the same model as sp_gc_heap/bytes.
   sp_gc_alloc itself is an external function (defined in sp_alloc.c) so the
   cold lib C files that already link it (sp_fiber.c, sp_io.c, sp_bigint.c)
   keep resolving the same symbol. */
/* SPINEL_GC_OBJ_BUDGET=walk: the object budget is priced off the whole set a
   mark walks, not the object heap alone. Opt-in; see sp_gc_retune_object. */
extern int sp_gc_obj_budget_mode;
extern size_t sp_gc_obj_alpha1024;
extern int sp_gc_str_major_fixed;
extern int sp_gc_str_major_sched;
extern size_t sp_gc_str_majors;
extern int sp_gc_obj_budget_fixed;
extern int sp_gc_str_budget_fixed;
extern size_t sp_gc_threshold;
extern size_t sp_gc_threshold_init;
extern int sp_gc_stress_checked;
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
void *sp_gc_alloc_nogc(size_t sz, void (*fin)(void *), void (*scn)(void *));

SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);  /* lib/sp_core.c */
SP_NORETURN void sp_raise_frozen_str(const char *s);              /* lib/sp_str.c */
/* The message carries the rodata marker byte: an in-flight exception's msg is
   marked by the GC (sp_mark_string reads s[-1]), so a bare literal -- whose
   [-1] is out of bounds -- would be UB when it lands at a section edge. */
static SP_NOINLINE SP_COLD void sp_raise_frozen_array(void) { sp_raise_cls("FrozenError", (&("\xff" "can't modify frozen Array")[1])); }
/* Same, but stages the receiver so FrozenError#receiver answers the frozen
   object itself (identity-preserving boxing of the mutation target) (#3002).
   sp_exc_stage_recv lives in the generated TU; the ctor transfers the staged
   value onto the raised exception's xrecv slot. */
void sp_exc_stage_recv(sp_RbVal v);
/* the raise itself, in lib/sp_cold.c: the message carries the receiver's
   inspect, as CRuby's does ("can't modify frozen Array: [1, 2]") */
SP_NORETURN void sp_raise_frozen_array_rv(sp_RbVal v);
static SP_NOINLINE SP_COLD void sp_raise_frozen_array_at(void *a, int cls_id) {
  sp_raise_frozen_array_rv(sp_box_obj(a, cls_id));
}
/* boxed-receiver variant (the mutator holds an sp_RbVal, not the raw ptr) */
static SP_NOINLINE SP_COLD void sp_raise_frozen_array_v(sp_RbVal v) {
  sp_raise_frozen_array_rv(v);
}

/* Top-level self, the main object (#4926): a bare Object allocated on first
   use, whose to_s / inspect answer "main". sp_main_obj is its pointer, a GC
   root marked with the runtime globals; lib/sp_cold.c. */
extern void *sp_main_obj;
sp_RbVal sp_main_self(void);

/* sp_PolyArray: a growable array of boxed values. The first
   SP_POLYARR_INLINE elements live in the object itself: an array that never
   outgrows them has no payload, so no finalizer, so the sweep never touches
   it dead (lib/sp_slab.c) -- and most arrays never do. Growing past them
   moves the elements to a payload and installs the finalizer that frees it. */
#define SP_POLYARR_INLINE 8
typedef struct { sp_RbVal *data; sp_int len; sp_int cap; sp_int frozen; sp_RbVal inl[SP_POLYARR_INLINE]; } sp_PolyArray;
static inline void sp_PolyArray_scan(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; for (sp_int i = 0; i < a->len; i++) sp_mark_rbval(a->data[i]); }
static inline void sp_PolyArray_fin(void *p) { sp_PolyArray *a = (sp_PolyArray *)p; if (a->data == a->inl) return; sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); sp_gc_bytes_sub(sizeof(sp_RbVal) * a->cap); h->size -= sizeof(sp_RbVal) * a->cap; sp_pl_free(a->data); }
/* Free-list pool for PolyArray, header AND data buffer kept together. The
   allocation-heaviest programs (per-point tuple building: BabyStark's
   constraint evaluation) churn millions of short-lived PolyArrays; recycling
   them turns the calloc+malloc pair and the sweep-side free into list ops.
   Pool state lives in sp_alloc.c; the recycle hook runs inside the sweep.
   With the slab on there is no pool: the slab's own allocation is as cheap
   as a pop, and a pooled header is one the sweep has to touch dead (to run
   its recycler) where an unpooled one dies in its chunk's bitmap. */
/* The pool is per thread (sp_alloc.c says why), so the pop is a plain
   list operation on both builds. */
extern SP_TLS sp_gc_hdr *sp_polyarr_pool_head;
extern SP_TLS long sp_polyarr_pool_count;
void sp_PolyArray_pool_recycle(sp_gc_hdr *h);
static inline sp_PolyArray *sp_PolyArray_new(void) {
  if (sp_slab_on > 0) {
    sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), NULL, sp_PolyArray_scan);
    a->data = a->inl; a->cap = SP_POLYARR_INLINE; a->len = 0;
    return a;
  }
  sp_gc_hdr *ph = sp_polyarr_pool_head;
  if (ph) { sp_polyarr_pool_head = ph->next; sp_polyarr_pool_count--; }
  if (ph) {
    /* back into the live heap (the sweep parked it); size still counts
       header + retained data buffer, so the byte accounting stays exact */
    ph->marked = 0; ph->old = 0; ph->dirty = 0;
    ph->recycle = sp_PolyArray_pool_recycle;
    if (sp_slab_owns(ph)) sp_slab_relive(ph);
    else SP_GC_HEAP_PUSH(ph);
    sp_gc_bytes_add(ph->size);
    sp_PolyArray *a = (sp_PolyArray *)((char *)ph + sizeof(sp_gc_hdr));
    a->len = 0;
    a->frozen = 0;
    return a;
  }
  sp_PolyArray *a = (sp_PolyArray *)sp_gc_alloc(sizeof(sp_PolyArray), sp_PolyArray_fin, sp_PolyArray_scan);
  a->data = a->inl; a->cap = SP_POLYARR_INLINE; a->len = 0;
  { sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr)); h->recycle = sp_PolyArray_pool_recycle; sp_slab_set_fin(h); }
  return a;
}
/* past the inline elements: the first growth moves them to a payload and
   gives the object the finalizer that will free it; later growths resize */
static SP_NOINLINE void sp_PolyArray_grow(sp_PolyArray *a) {
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)a - sizeof(sp_gc_hdr));
  sp_int nc = (a->cap * 2) + 1;
  if (a->data == a->inl) {
    sp_RbVal *nd = (sp_RbVal *)sp_pl_alloc(sizeof(sp_RbVal) * (size_t)nc);
    memcpy(nd, a->inl, sizeof(sp_RbVal) * (size_t)a->len);
    a->data = nd;
    if (!h->finalize) { h->finalize = sp_PolyArray_fin; sp_slab_set_fin(h); }
  }
  else {
    sp_gc_bytes_sub(sizeof(sp_RbVal) * a->cap); h->size -= sizeof(sp_RbVal) * a->cap;
    a->data = (sp_RbVal *)sp_pl_realloc(a->data, sizeof(sp_RbVal) * (size_t)nc);
  }
  a->cap = nc;
  h->size += sizeof(sp_RbVal) * a->cap; sp_gc_bytes_add(sizeof(sp_RbVal) * a->cap);
}
static inline void sp_PolyArray_push(sp_PolyArray *a, sp_RbVal v) { if (!a) return; sp_gc_wb((void*)a); if (a->frozen) { sp_raise_frozen_array(); return; } if (a->len >= a->cap) sp_PolyArray_grow(a); a->data[a->len++] = v; }
static inline sp_RbVal sp_PolyArray_get(sp_PolyArray *a, sp_int i) { if (!a) return sp_box_nil(); if ((unsigned long long)i < (unsigned long long)a->len) return a->data[i]; if (i < 0) i += a->len; if (i < 0 || i >= a->len) return sp_box_nil(); return a->data[i]; }
/* ---- relocated from spinel_rt.h: frozen-string check primitives used
   by lib/sp_cold.c's sp_str_setbyte_cow, and the SPL frozen-literal macro
   used by lib/sp_cold.c's sp_gc_stat. Pure textual move (still static
   inline / object-like macro), no codegen change. ---- */
#define SPL(s) (&("\xff" s)[1])
/* 0xf1 = heap string frozen by an explicit .freeze call.
   Unlike 0xff (rodata literal) this marker lives in a malloc'd buffer
   so sp_str_freeze_val can set it.  The frozen? predicate and mutation
   guards check for 0xf1; plain rodata 0xff literals are NOT reported
   as frozen (they behave as immutable value-semantics objects).
   0xfa is a frozen heap string that is collected like any other where 0xf1
   is kept for good; 0xf8 is its marked state on a malloc'd string, as 0xfc
   is 0xfe's (a slab string's mark is a bit in its chunk). */
static inline sp_bool sp_str_is_frozen_val(const char *s) {
  if (!s) return TRUE;
  unsigned char m = ((const unsigned char *)s)[-1];
  return m == 0xf1 || m == 0xfa || m == 0xf8;
}
static inline void sp_str_check_mutable(const char *s) {
  if (sp_str_is_frozen_val(s)) sp_raise_frozen_str(s);
}
/* A String key as a Hash stores it: a mutable string is copied into a frozen
   0xfa, so a later `<<` or setbyte on the caller's string cannot move the key
   (CRuby's rb_hash_key_str). A frozen or literal key is stored as is. The
   copy can collect, which clears the write barrier's record of the hash:
   the caller issues its barrier again before storing. */
static inline const char *sp_hash_key_str(const char *s) {
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m != 0xfe && m != 0xfc && m != 0xfd) return s;
  char *r = (char *)sp_str_from_bytes(s, sp_str_byte_len(s));
  if (sp_str_is_binary(s)) sp_str_mark_binary(r);
  ((unsigned char *)r)[-1] = 0xfa;
  return r;
}

/* ---- relocated from spinel_rt.h: integer add/sub/mul overflow-check
   helpers (still static inline, pure textual move) used by lib/sp_cold.c's
   sp_int_pow. ---- */
/* the builtin, C23 <stdckdint.h> or a range check, per sp_compat.h */
static inline sp_bool sp_int_add_overflow_p(sp_int a, sp_int b, sp_int *r) {
  return sp_ckd_add_iptr(a, b, r);
}
static inline sp_bool sp_int_sub_overflow_p(sp_int a, sp_int b, sp_int *r) {
  return sp_ckd_sub_iptr(a, b, r);
}
static inline sp_bool sp_int_mul_overflow_p(sp_int a, sp_int b, sp_int *r) {
  return sp_ckd_mul_iptr(a, b, r);
}

/* ---- Integer leaf-op prototypes (bodies relocated to lib/sp_cold.c):
   chr/digits/bit_length/bit_range/to_s_base/opt variants/pow. ---- */
const char *sp_int_chr(sp_int n);
const char *sp_int_chr_utf8(sp_int n);
const char *sp_int_codepoint_to_str(sp_int n);
const char *sp_int_codepoint_to_str_in(const char *recv, sp_int n);
sp_IntArray *sp_int_digits(sp_int n, sp_int base);
sp_int sp_int_bit_length(sp_int n);
sp_int sp_int_bit_range(sp_int n, sp_int start, sp_int len);
const char *sp_int_interp(sp_int n);
const char *sp_int_to_s_base(sp_int n, sp_int base);
const char *sp_int_opt_inspect(sp_int v);
const char *sp_int_opt_to_s(sp_int v);
sp_int sp_int_pow(sp_int base, sp_int exp);

/* ---- Float leaf-op prototypes (bodies relocated to lib/sp_cold.c). ---- */
const char *sp_float_opt_inspect(sp_float v);
const char *sp_float_opt_to_s(sp_float v);
sp_int sp_float_denominator(sp_float f);
sp_RbVal sp_float_numerator(sp_float f);
/* Float#to_i: an in-range, non-NaN value is a C truncation, done inline;
   NaN (which fails both tests), Infinity and an out-of-range value go to the
   raising body in sp_cold.c. The range is exactly the one that body checks. */
sp_int sp_float_to_i_checked_slow(sp_float f);
static inline sp_int sp_float_to_i_checked(sp_float f) {
  if (SP_LIKELY(f >= (sp_float)INTPTR_MIN && f < -(sp_float)INTPTR_MIN)) return (sp_int)f;
  return sp_float_to_i_checked_slow(f);
}

/* ---- forward declarations for pointer-only box params (full types stay
   opaque to lib/sp_alloc.h -- these box functions only store the pointer). ---- */
typedef struct sp_Bigint sp_Bigint;               /* full def: spinel_rt.h bigint block */
typedef struct sp_OpenStruct_s sp_OpenStruct;      /* full def: spinel_rt.h (SymPolyHash-backed) */
typedef struct { sp_float utime, stime, cutime, cstime; } sp_Tms;
/* Addrinfo: one resolved endpoint. Immutable; the strings are GC-managed. */
typedef struct {
  const char *ip;        /* numeric address, or the path for AF_UNIX */
  const char *afname;    /* "AF_INET" / "AF_INET6" / "AF_UNIX", for #inspect */
  sp_int afamily;       /* the AF_* value, which is what #afamily answers */
  sp_int port;
  sp_int socktype;      /* SOCK_STREAM / SOCK_DGRAM */
  sp_int protocol;
} sp_Addrinfo;
/* Socket::Option: what #getsockopt answers. Spinel carries the integer-valued
   options only, so the payload is the int itself rather than a byte string. */
typedef struct { sp_int family, level, optname, value; } sp_SockOpt;

/* ---- Box/Encoding helpers relocated from spinel_rt.h: hot-ish ones
   (sp_box_class 7x / sp_box_nullable_obj 64x / sp_box_int_array 24x /
   sp_box_float_array 12x / sp_box_str_array 5x / sp_box_range 4x in
   optcarrot) stay static inline (pure textual move, no codegen change);
   sp_encoding_name/_inspect/_eq (0 uses) ride along since sp_box_encoding
   needs sp_encoding_name. ---- */
static inline sp_RbVal sp_box_class_name(const char *name) { sp_RbVal r; r.tag = SP_TAG_CLASS; r.cls_id = SP_CLASS_BY_NAME; r.v.s = name; return r; }
/* box a sp_Class into a poly slot (a name-backed class boxes by name). */
static inline sp_RbVal sp_box_class(sp_Class c) { if (sp_class_nil_p(c)) return sp_box_nil(); if (c.name) return sp_box_class_name(c.name); sp_RbVal r; r.tag = SP_TAG_CLASS; r.cls_id = (int)c.cls_id; r.v.i = c.cls_id; return r; }
/* Regexp.escape(s) / Regexp.quote(s) -- prefix every regex metachar
   and whitespace byte with a single backslash, returning a heap
   string that callers can feed into `Regexp.new(...)` to match the
   original bytes literally. Matches CRuby's rb_reg_quote for the
   ASCII range (the only range Spinel's regex engine indexes today,
   so multibyte passes through unchanged).

   The metachars covered: \\ . ? * + ^ $ | ( ) [ ] { } # -
   The whitespace covered: ' ' \t \n \r \f \v
   Everything else copies byte-for-byte. */
static inline sp_RbVal sp_box_nullable_obj(void *p, int cls_id) { return p ? sp_box_obj(p, cls_id) : sp_box_nil(); }
/* Same, for a class that has subclasses: the id the object itself carries (its
   first field) beats the static one, so an inherited method boxing `self`
   publishes the actual class rather than the one that defined the method. */
static inline sp_RbVal sp_box_nullable_obj_dyn(void *p, int cls_id) {
  (void)cls_id;
  return p ? sp_box_obj(p, (int)*(sp_int *)p) : sp_box_nil();
}
/* Built-in pointer boxes -- share SP_TAG_OBJ with a reserved negative
   cls_id so the dispatch path is uniform. */
static inline sp_RbVal sp_box_int_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_INT_ARRAY); }
static inline sp_RbVal sp_box_float_array(void *p) { return sp_box_obj(p, SP_BUILTIN_FLT_ARRAY); }
static inline sp_RbVal sp_box_str_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_STR_ARRAY); }
/* sp_Range is a 16-byte value type that doesn't fit in sp_RbVal's union
   (max 8 bytes). When a Range crosses into a poly slot (heterogeneous
   hash / array / param / ivar), copy it onto the GC heap and box the
   pointer via SP_BUILTIN_RANGE. The Range has no internal pointer fields
   so no scanner is needed. */
static inline sp_RbVal sp_box_range(sp_Range v) {
  sp_Range *p = (sp_Range *)sp_gc_alloc(sizeof(sp_Range), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_RANGE);
}
static inline const char*sp_encoding_name(sp_Encoding e){return e.name?e.name:sp_str_empty;}
static inline const char*sp_encoding_inspect(sp_Encoding e){return sp_sprintf("#<Encoding:%s>",sp_encoding_name(e));}
static inline sp_bool sp_encoding_eq(sp_Encoding a,sp_Encoding b){const char*an=sp_encoding_name(a);const char*bn=sp_encoding_name(b);return strcmp(an,bn)==0;}

/* ---- Box helper prototypes (0 optcarrot uses; bodies in lib/sp_cold.c). ---- */
/* An int? / float? value crossing into a poly slot: the sentinel is nil, never
   a number. Inline because every boxed element read goes through here. */
static inline sp_RbVal sp_box_int_or_nil(sp_int v) { return v == SP_INT_NIL ? sp_box_nil() : sp_box_int(v); }
static inline sp_RbVal sp_box_float_or_nil(sp_float v) { return sp_float_is_nil(v) ? sp_box_nil() : sp_box_float(v); }
/* An element of an Integer or Float array boxed for a poly container: `nf` is
   the array's may_nil, read once ahead of the loop, so an array that cannot
   hold nil boxes its elements as they are. */
#define sp_box_int_nf(nf, v) ((nf) ? sp_box_int_or_nil(v) : sp_box_int(v))
#define sp_box_float_nf(nf, v) ((nf) ? sp_box_float_or_nil(v) : sp_box_float(v))
sp_RbVal sp_unsentinel(sp_RbVal v);
sp_RbVal sp_box_bigint(sp_Bigint *b);
/* A bigint slot's nil is NULL (the compiler's nil_value for TY_BIGINT), so a
   nilable bigint boxes as nil rather than as a truthy Integer printing 0. */
static inline sp_RbVal sp_box_bigint_or_nil(sp_Bigint *b) { return b ? sp_box_bigint(b) : sp_box_nil(); }
/* A 64-bit value as a Ruby Integer: the sp_int when it fits, a Bignum when
   it does not (a checksum, an unpacked quad, a parsed literal on a 32-bit
   sp_int). What a package answers as :any for a value that may be wide. */
sp_RbVal sp_box_i64(int64_t v);
/* the inverse: a boxed Integer (or Float) as a 64-bit value, a Bignum through
   its low 64 bits, for a package parameter that may be wider than sp_int */
int64_t sp_unbox_i64(sp_RbVal v);
sp_RbVal sp_box_encoding(sp_Encoding e);
sp_RbVal sp_encoding_find(sp_RbVal v);
sp_RbVal sp_box_nullable_str(const char *v);
sp_RbVal sp_box_foreign_ptr(void *p);
sp_RbVal sp_box_regexp(void *p);
sp_RbVal sp_box_sym_array(void *p);
sp_RbVal sp_box_ptr_array(void *p);
/* Box a pointer array by reference, stamping what its elements are so the poly
   paths can read, render and refuse stores faithfully (#4486). */
static inline sp_RbVal sp_box_ptr_array_k(void *p, int kind, int cls) {
  if (!p) return sp_box_nil();
  sp_PtrArray *a = (sp_PtrArray *)p;
  if (a->elem_kind == SP_PTR_ELEM_UNKNOWN) { a->elem_kind = kind; a->elem_cls = cls; }
  return sp_box_obj(p, SP_BUILTIN_PTR_ARRAY);
}
sp_RbVal sp_box_method(void *p);
sp_RbVal sp_box_complex(sp_Complex v);
sp_RbVal sp_box_rational(sp_Rational v);
sp_RbVal sp_box_time(sp_Time v);
sp_RbVal sp_box_tms(sp_Tms v);
struct sp_ProcessStatus_s;   /* forward decl for the box helper */
sp_RbVal sp_box_process_status(struct sp_ProcessStatus_s *v);
sp_RbVal sp_box_addrinfo(sp_Addrinfo *v);
sp_RbVal sp_box_sockopt(sp_SockOpt *v);
void sp_addrinfo_scan(void *p);
sp_RbVal sp_box_openstruct(sp_OpenStruct *o);

/* ---- class-frozen bitmap (Class#freeze / #frozen?): state stays
   per-process like sp_argv, extern instead of spinel_rt.h-static. ---- */
extern unsigned char sp_class_frozen_map[4096];   /* one definition: lib/sp_cold.c */
void sp_class_freeze_id(sp_int cls_id);
sp_bool sp_class_frozen_id(sp_int cls_id);

/* ---- rounding helpers relocated from spinel_rt.h (0 optcarrot uses). ---- */
double sp_round_half_even(double x);
double sp_round_half_down(double x);

/* ---- BigRational (Bignum-denominator Rational, #2469): 0 optcarrot
   uses. sp_Bigint is forward-declared above (box_bigint); the specific
   sp_bigint_* ops below are resolved at the final link against the
   generated TU, same as sp_sprintf. ---- */
typedef struct { sp_Bigint *num, *den; } sp_BigRational;
int sp_bigint_sign(sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_new_int(int64_t v);
int64_t sp_bigint_to_int(sp_Bigint *b);
sp_Bigint *sp_bigint_gcd(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
char *sp_str_alloc_ext(size_t len);   /* non-inline sp_str_alloc; see sp_alloc.c */
const char *sp_bigint_to_s(sp_Bigint *b);
double sp_bigint_to_double(sp_Bigint *b);
void sp_brat_scan(void *p);
sp_RbVal sp_box_brat(sp_Bigint *num, sp_Bigint *den);
sp_RbVal sp_brat_from_bigint(sp_Bigint *n);
const char *sp_brat_to_s(sp_BigRational *r);
const char *sp_brat_inspect(sp_BigRational *r);
sp_float sp_brat_to_f(sp_BigRational *r);
/* the exact integer quotients, each rounding the way its method does */
sp_Bigint *sp_brat_trunc_b(sp_BigRational *r);
sp_Bigint *sp_brat_floor_b(sp_BigRational *r);
sp_Bigint *sp_brat_ceil_b(sp_BigRational *r);
sp_Bigint *sp_brat_round_b(sp_BigRational *r);

/* ---- Marshal.dump/load helpers (lib/sp_marshal.c calls these): 0
   optcarrot uses. sp_marv_hash_new/set stay in spinel_rt.h instead of
   moving here -- they need sp_PolyPolyHash_new/set, which are hot
   (called there dozens of times) and
   whose home is the struct's own definition deep in spinel_rt.h, not
   this early header; de-static'ing them in place to reach two
   one-line marv wrappers would grow spinel_rt.h's non-static-body
   count for no real gain, so those two stay put instead. */
sp_RbVal sp_marv_arr_new(void);
void sp_marv_arr_push(sp_RbVal a, sp_RbVal v);
sp_RbVal sp_marv_box_complex(sp_float re, sp_float im);
sp_RbVal sp_marv_box_rational(sp_int n, sp_int d);
void sp_marv_raise(const char *cls, const char *msg);

/* ---- FFI array data pointers, array-kind length probe, sp_Class
   unboxing: relocated from spinel_rt.h (0 optcarrot uses). ---- */
const int64_t *sp_ffi_int_array_data(sp_RbVal v);
const double *sp_ffi_float_array_data(sp_RbVal v);
const int64_t *sp_PolyArray_ffi_int_data(sp_PolyArray *a);
const double *sp_PolyArray_ffi_float_data(sp_PolyArray *a);
sp_int sp_array_kind_len(sp_RbVal el);
sp_Class sp_unbox_class(sp_RbVal v);

#endif /* SP_ALLOC_H */
