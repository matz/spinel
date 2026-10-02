/* sp_gc.h -- the mark/sweep collector's shared surface.
 *
 * Included by both the generated translation unit (via spinel_rt.h) and
 * lib/sp_gc.c, which holds the collector's non-inline machinery (mark,
 * sweep, collect, the memory-limit governor, and the SPINEL_GC_VERIFY
 * support). The hot inline mark helpers stay here so both sides inline
 * them -- moving the cold collector body to a single compiled unit must
 * not de-inline the per-object mark path. The collector globals are
 * declared extern here and defined once in lib/sp_gc.c.
 */
#ifndef SP_GC_H
#define SP_GC_H

#include <stddef.h>
#include "sp_types.h"

/* ---- Value tag constants + the boxed value (sp_RbVal) ----
 * The mark helpers below dispatch on the tag, so the type lives here
 * rather than in the generated TU. */
#define SP_TAG_INT 0
#define SP_TAG_STR 1
#define SP_TAG_FLT 2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL 4
#define SP_TAG_OBJ 5
#define SP_TAG_SYM 6
#define SP_TAG_CLASS 7
#define SP_TAG_ENCODING 8
#define SP_TAG_BIGINT 9   /* v.p is a GC-allocated sp_Bigint* */
/* SP_TAG_OBJ cls_id sentinel for an opaque foreign/FFI pointer (e.g. a
   ffi_read_ptr / ffi func ptr return). It is NOT a sp_gc_alloc allocation, so
   the collector must not trace it -- sp_mark_rbval skips it. Kept here (not with
   the other SP_BUILTIN_* in spinel_rt.h) so the inline mark helper can see it.
   Value is the next free slot below SP_BUILTIN_METHOD (-24). */
#define SP_BUILTIN_FOREIGN_PTR (-25)
/* a compiled Regexp: malloc-owned (never GC heap), so like FOREIGN_PTR the
   collector must not trace it */
#define SP_BUILTIN_REGEX       (-33)
/* ARGF: the one static sp_argf_obj, never GC heap, so it is not traced */
#define SP_BUILTIN_ARGF        (-51)
/* Wide value types (heap-copied crossing into a poly slot). Shared here so
   lib/sp_marshal.c can recognize them by cls_id. */
#define SP_BUILTIN_COMPLEX  (-26)
#define SP_BUILTIN_RATIONAL (-27)
/* A Rational whose numerator/denominator exceed sp_int: a boxed object with
   two sp_Bigint* fields, distinct from the by-value int Rational (#2469). */
#define SP_BUILTIN_BIG_RATIONAL (-35)
/* A Float range (1.0..3.0): a boxed sp_FloatRange, distinct from the int-backed
   by-value Range so its endpoints are not truncated. */
#define SP_BUILTIN_FLOAT_RANGE (-36)
#define SP_BUILTIN_STR_RANGE   (-42)  /* ("a".."e"): sp_StrRange. Distinct from
                                         STRBUF/ADDRINFO, which -40 already
                                         names: a boxed string range answered
                                         String from every tag-keyed switch,
                                         so it was neither a Range nor
                                         enumerable through a poly slot (#3619) */
#define SP_BUILTIN_OPENSTRUCT  (-41)  /* OpenStruct: dynamic symbol->value members */
typedef struct { int tag; int cls_id; union { sp_int i; const char *s; sp_float f; sp_bool b; void *p; } v; } sp_RbVal;

/* ---- Collector globals shared with the generated TU ----
 * Only the globals touched by both the kept hot path (sp_gc_alloc, the
 * SP_GC_ROOT macros, GC.stat, the fiber root hook) and the moved cold
 * body are extern; the rest stay static on whichever side owns them. */
/* Capacity of the stack-resident GC root array (sp_gc_roots). At 8 bytes/entry
   the default is a 512 KB static buffer -- ample for desktop, but the dominant
   static allocation in a minimal binary. Embedded targets can shrink it with
   -DSP_GC_STACK_MAX=<n> (pass the SAME value when building lib/sp_gc.c and the
   generated TU -- both consult this for the array and the SP_GC_ROOT bound).
   A push past it goes to a heap-allocated overflow segment (sp_gc_root_push_slow),
   so a recursion deeper than the array -- 27k Ruby frames of a growing
   string reached it, and the dropped roots were freed under the frames that
   still held them -- keeps every root; only the fast path's bound is this
   constant. */
#ifndef SP_GC_STACK_MAX
#define SP_GC_STACK_MAX 65536
#endif
#define SP_GC_FULL_INTERVAL 8
/* Per-worker root stack (SP_TLS): each OS worker carries the active roots of
   the green thread it runs, swapped with the fiber's saved_roots on a context
   switch. Plain globals in the single-threaded build. */
extern SP_TLS void **sp_gc_roots[SP_GC_STACK_MAX];
extern SP_TLS int sp_gc_nroots;
extern SP_TLS int sp_gc_in_sweeper;   /* set on a sweeper thread: finalizers skip the per-worker byte accounting */

/* GC root tracking. SP_GC_ROOT registers a stack-resident root with a
   cleanup-attribute sentinel so it auto-pops when its declaring scope ends.
   Shared here (was in spinel_rt.h) so standalone lib C files -- e.g. the
   Marshal loader, which builds GC arrays/hashes across a recursive parse --
   can root their in-flight objects too. Helpers touch only the extern root
   stack above, so relocating them is layout-neutral. */
/* Root i, whichever segment holds it: the walkers and the fiber snapshot read
   through this past SP_GC_STACK_MAX. */
extern SP_TLS void ***sp_gc_roots_ext;
int sp_gc_root_push_slow(void **p);
int sp_gc_roots_ext_reserve(int n);   /* room for n roots in total; 0 on OOM */
static inline void **sp_gc_root_at(int i) {
  return i < SP_GC_STACK_MAX ? sp_gc_roots[i] : sp_gc_roots_ext[i - SP_GC_STACK_MAX];
}
static inline int _sp_gc_root_push(void **p) {
  if (sp_gc_nroots < SP_GC_STACK_MAX) { sp_gc_roots[sp_gc_nroots++] = p; return 1; }
  return sp_gc_root_push_slow(p);
}
static inline void _sp_gc_root_pop(int *added) { if (*added) sp_gc_nroots--; }
static inline void sp_gc_cleanup(int *p) { sp_gc_nroots = *p; }
#define _SP_GC_CONCAT2(a,b) a##b
#define _SP_GC_CONCAT(a,b) _SP_GC_CONCAT2(a,b)
#define SP_GC_SAVE() int SP_CLEANUP(sp_gc_cleanup) _gc_saved = sp_gc_nroots
/* A `const char *` slot is a String, and takes the string tag (below) whichever
   macro roots it: a mutable String's payload (marker 0xfd) is kept alive by
   the handle in front of it, which only sp_mark_string reaches, and the
   object walk skips the payload. The slot's C type is the one thing every
   emitter agrees on, so the choice is made here rather than at each site. */
#define _SP_GC_SLOT_TAG(v) _Generic(&(v), const char **: (uintptr_t)2, default: (uintptr_t)0)
#define SP_GC_ROOT(v) int SP_CLEANUP(_sp_gc_root_pop) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | _SP_GC_SLOT_TAG(v)))
/* Root a poly (sp_RbVal) local: tag the stored slot's low bit so the mark
   walker routes it through sp_mark_rbval (the object pointer sits in a union at
   a nonzero offset, only for STR/OBJ tags). */
#define SP_GC_ROOT_RBVAL(v) int SP_CLEANUP(_sp_gc_root_pop) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | (uintptr_t)1))
/* Root a string slot that may hold a NON-spinel pointer (a stack line
   buffer from sp_File_gets_buf, an external char*): tag bit 2 routes the
   mark through sp_mark_string, which touches nothing unless the marker
   byte is exactly 0xfe -- safe on arbitrary memory, unlike sp_gc_mark's
   header walk. Use this for string parameters in runtime helpers. */
#define SP_GC_ROOT_STR(v) int SP_CLEANUP(_sp_gc_root_pop) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | (uintptr_t)2))
#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved

/* ---- root frames ----
   A generated function registers ONE entry for all of its roots instead of
   one per root: a stack struct headed by sp_gc_frame_hdr, followed by nv
   sp_RbVal slots that are the homes of the function's boxed temporaries, then
   np entries in the same tagged-pointer encoding as the root array itself
   (SP_GC_ENTRY_*) for the locals rooted at function scope. Tag 3 -- the one
   value the per-root tags 1 and 2 never combine to -- marks the entry, so the
   walkers' existing `& 3` test already routes it to sp_gc_mark_root_entry.

   The per-root form costs the C compiler an address-taken stack slot, a
   bounds check and a cleanup-attribute pop duplicated on every scope exit;
   on a large generated TU those were a third of clang's unoptimised IR and
   most of its peak memory. The frame is one alloca, zeroed once at entry
   (a zero slot is INT 0, which marks nothing), popped by SP_GC_SAVE's cleanup
   with everything above it. A slot keeps its last value until the function
   returns, so an object a dead temporary named is held that much longer --
   an over-approximation of liveness, never an under-approximation. The
   layout is decoded in sp_gc_mark_frame; the generated struct is
   `{ sp_gc_frame_hdr h; sp_RbVal v[nv]; void **p[np]; }`, either array
   omitted when its count is 0. */
typedef struct { int nv; int np; } sp_gc_frame_hdr;
#define SP_GC_ROOT_FRAME(f) ((void)_sp_gc_root_push((void**)((uintptr_t)&(f) | (uintptr_t)3)))
#define SP_GC_ENTRY_PTR(v)   ((void**)((uintptr_t)&(v) | _SP_GC_SLOT_TAG(v)))
#define SP_GC_ENTRY_RBVAL(v) ((void**)((uintptr_t)&(v) | (uintptr_t)1))
#define SP_GC_ENTRY_STR(v)   ((void**)((uintptr_t)&(v) | (uintptr_t)2))

/* ---- write barrier ----
   A generational mark walks the young objects and whatever the roots reach; an
   old object it does not walk can still be the only thing holding a young one.
   The barrier records those: when a reference is stored into an object that has
   already been promoted, that object joins the remembered set, which a minor
   collection treats as an extra root.

   Cost is one load of a header bit already on the store's own cache line and a
   branch that steady state does not take -- measured at 0.5% on optcarrot when
   it fires on reference stores only, and 14% when it fires on every ivar store
   including the scalar ones, which is why the emitter discriminates.

   The set is a plain array with a dirty bit for deduplication. Overflow is
   safe rather than fatal: a full set means the next collection marks whole-heap
   (sp_gc_rem_overflow), which is exactly what today's collector always does. */
/* Wrap the OBJECT of a reference store: runs the barrier and yields the object
   itself, so it works wherever the store appears -- a statement, or an
   assignment inside a larger expression. The statement expression evaluates the
   object once, which a comma form would not. */
#define SP_WBO(x) ({ __typeof__(x) _sp_wbo = (x); sp_gc_wb((void *)_sp_wbo); _sp_wbo; })
#define SP_GC_REMEMBERED_MAX 65536
extern void *sp_gc_remembered[SP_GC_REMEMBERED_MAX];
extern int sp_gc_nremembered;
extern int sp_gc_rem_overflow;
extern int sp_gc_minor_on;   /* read by sp_gc_wb below; set once before main */
/* SPINEL_GC_OBJ_BUDGET=walk: the object collection budget is priced off the
   whole set a mark walks (objects + strings) rather than the object heap
   alone. Set once before main, beside the modes above; read by
   sp_gc_retune_object, which is where the reasoning lives. */
extern int sp_gc_obj_budget_mode;   /* 0 obj, 1 walk, 2 gated (default) */
extern size_t sp_gc_obj_alpha1024;  /* the last gate decision, in 1024ths */
extern int sp_gc_str_major_fixed;
extern int sp_gc_str_major_sched;
extern size_t sp_gc_str_majors;
extern int sp_gc_obj_budget_fixed;
extern int sp_gc_str_budget_fixed;
/* Set for the duration of the string sweep hook on a minor cycle: only the
   young string list may be swept, because the mark that just ran did not
   walk old objects and so did not reach the strings they hold. */
extern int sp_gc_str_minor_only;
/* The barrier proper. Out of line and behind the mode test: with the minor
   mark off, which is the default, every store site pays one predictable
   branch instead of carrying the tag protocol and the remembered-set push
   inline. Inlining all of it cost ~5% on optcarrot, whose inner loops write
   object references per scanline. */
void sp_gc_wb_slow(void *obj);
/* The STICKY half of the remembered set. `sp_gc_wb` records a store the
   barrier saw; this records a HOLDER whose stores it will not see, so the
   entry is kept for as long as the object lives rather than cleared each
   cycle.

   The one producer is a by-reference String parameter. The callee stores
   through `const char **_cell_x` and cannot name what owns that slot -- the
   caller may have lent a stack local, a heap cell, a proc's capture slot or
   an ivar -- and reading a header off a stack address to find out is exactly
   the fault that took #4391's first half down. The owner IS nameable at the
   lending call site, but that site runs BEFORE the store, and a barrier
   before a call that can collect does not cover the stores after it (#4378).
   Sticky is what makes the placement stop mattering.

   Only the two forms that need it are lent: a heap cell and an ivar's owner.
   A stack local is lent as `&lv_x`, and the caller's frame roots it for the
   whole nest below, so nothing is pinned for it -- which is also why
   FORWARDING a by-reference parameter pins nothing: whatever the original
   lending site was, it already decided. */
void sp_gc_pin_remembered_slow(void *obj);
static inline void sp_gc_pin_remembered(void *obj) {
  if (SP_EXPECT(sp_gc_minor_on, 1)) sp_gc_pin_remembered_slow(obj);
}
#define SP_GC_PINNED_MAX 16384
extern void *sp_gc_pinned[SP_GC_PINNED_MAX];
extern int sp_gc_npinned;
extern int sp_gc_pin_overflow;
static inline void sp_gc_wb(void *obj) {
  /* Nothing reads the remembered set unless a minor mark runs, and whether one
     can is decided once, from the environment, before main. So with the
     generational mark off -- the default -- the whole barrier is bookkeeping
     for a reader that never comes. rubys observed the other half of this from
     the source: `old` is set on every survivor regardless of the mode, so the
     barrier was doing its full work in both. */
  if (SP_EXPECT(sp_gc_minor_on, 1)) {
    /* With the mark on, the common case -- a young holder, or an old one
       already recorded -- is decided here from the header the store is about
       to touch anyway, so the call is paid only by a store that actually
       records something. The tag byte says whether there is a header at all
       (the same protocol sp_gc_wb_slow and sp_gc_mark read). */
    if (!obj) return;
    unsigned char pm = ((const unsigned char *)obj)[-1];
    if (pm == 0xfd || pm == 0xff || pm == 0xf1 || pm == 0xf0 ||
        pm == 0xfe || pm == 0xfc || pm == 0xfb || pm == 0xfa || pm == 0xf8) return;
    const sp_gc_hdr *h = (const sp_gc_hdr *)obj - 1;
    if (!h->old || h->dirty) return;
    sp_gc_wb_slow(obj);
  }
}
/* Young object heap. Threaded build: per-worker lists (one pusher each, since a
   started thread is pinned to its worker), so allocation pushes without the
   CAS-on-shared-head that made object-heavy parallel workloads bounce a cache
   line every alloc. Removals happen only under stop-the-world (every mutator
   parked). Survivors promote into the single shared old heap during the sweep,
   under stop-the-world, so the old list stays lock-free and shared. The live-
   byte counter sp_gc_bytes stays a single (relaxed-atomic) total -- array data
   buffers adjust it from whichever worker mutates them, which a per-worker split
   could not attribute correctly. */
#ifdef SP_THREADS
/* One cache-line-padded slot per worker holding the two fields written on every
   object allocation: the young list head and the unflushed live-byte delta.
   Padding is essential -- without it adjacent workers' 8-byte slots share a
   cache line, so a per-worker heap still bounced that line every alloc (false
   sharing), which kept object-heavy parallel allocation from scaling despite the
   lock/CAS removal. One line per worker isolates them completely. */
#define SP_CACHELINE 64
typedef struct {
  sp_gc_hdr *young;     /* per-worker young list head */
  size_t flush_delta;   /* per-worker unflushed live-byte delta (see below) */
  char _pad[SP_CACHELINE - sizeof(sp_gc_hdr*) - sizeof(size_t)];
} sp_gc_wslot_t;
extern sp_gc_wslot_t sp_gc_wslot[SP_MAX_WORKERS];
#else
extern sp_gc_hdr *sp_gc_heap;
#endif
/* Current mark generation (see sp_gc_hdr.marked in sp_types.h). */
extern unsigned sp_gc_mark_gen;
extern void (*sp_gc_obj_retune_hook)(size_t before);
extern size_t sp_gc_bytes;
extern size_t sp_gc_old_bytes;
extern int sp_gc_cycle;
/* SPINEL_GC_STATS=1 (report in sp_alloc.c): how many collections ran and what
   they cost. A program whose GC share of CPU climbs with concurrency looks
   from outside the process exactly like one collecting more often, and there
   was no counter to tell the two apart -- which is where the diagnosis in
   #4352 stopped. One clock pair and two increments per COLLECTION, so they
   are always kept; only the printing is gated. */
extern unsigned long long sp_gc_stat_collections;
extern unsigned long long sp_gc_stat_fulls;
extern double sp_gc_stat_seconds;
extern void (*sp_gc_mark_suspended_fibers_hook)(void);

/* Heap byte-counter accounting. The container growth paths (sp_array.h,
   sp_alloc.h's PolyArray, the string builder) adjust sp_gc_bytes inline
   WITHOUT the heap lock -- at N>1 workers that is a data race against the
   locked allocators and against each other (torn counters skew the GC
   trigger; TSan flags it). Under SP_THREADS every update and every
   trigger-decision read goes through a relaxed atomic: the counter is a
   heuristic (collection thresholds), so relaxed ordering is enough -- no
   other data is published through it. Collector-side code that runs under
   stop-the-world (sweep, retune) keeps plain accesses: every mutator is
   parked at the barrier, which already gives the happens-before edge.
   The single-threaded build expands to the exact plain +=/-= it had, so
   that archive stays byte-identical. */
#ifdef SP_THREADS
#define SP_GC_CTR_ADD(ctr, n) SP_ATOMIC_FETCH_ADD(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#define SP_GC_CTR_SUB(ctr, n) SP_ATOMIC_FETCH_SUB(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#define SP_GC_CTR_GET(ctr)    SP_ATOMIC_LOAD(&(ctr), __ATOMIC_RELAXED)
#define SP_GC_CTR_SET(ctr, n) SP_ATOMIC_STORE(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#else
#define SP_GC_CTR_ADD(ctr, n) ((ctr) += (size_t)(n))
#define SP_GC_CTR_SUB(ctr, n) ((ctr) -= (size_t)(n))
#define SP_GC_CTR_GET(ctr)    (ctr)
#define SP_GC_CTR_SET(ctr, n) ((ctr) = (size_t)(n))
#endif

/* Live-byte accounting for the object heap. Every allocation and every array-
   buffer resize adjusts sp_gc_bytes; under SP_THREADS a shared atomic RMW per
   op bounces one cache line across workers and dominated object-heavy parallel
   allocation (measured ~13x on the counter alone at 4 workers). Batch it: each
   worker accumulates its delta in a private (non-atomic) slot and flushes to the
   shared total only every SP_GC_FLUSH_QUANTUM, cutting the atomic frequency by
   ~quantum/alloc-size. sp_gc_bytes stays the authoritative shared total -- every
   read (threshold trigger, GC.stat) and the collector's recompute are unchanged;
   the trigger merely lags the true total by at most quantum*workers, bounded
   overshoot for a heuristic. The collector resets the deltas after its recompute
   (which already counts every live object's size, so the pending deltas are
   subsumed). The single-threaded build is the plain +=/-= it always was. */
#ifdef SP_THREADS
#define SP_GC_FLUSH_QUANTUM (16u * 1024u)
static inline void sp_gc_bytes_add(size_t n) {
  if (sp_gc_in_sweeper) return;   /* a finalizer on a sweeper thread: the mark already counted the live total */
  size_t *d = &sp_gc_wslot[sp_worker_id].flush_delta;
  size_t v = *d + n;
  if (v >= SP_GC_FLUSH_QUANTUM) { SP_GC_CTR_ADD(sp_gc_bytes, v); *d = 0; }
  else *d = v;
}
static inline void sp_gc_bytes_sub(size_t n) {
  if (sp_gc_in_sweeper) return;
  size_t *d = &sp_gc_wslot[sp_worker_id].flush_delta;
  if (*d >= n) { *d -= n; }
  else { size_t rem = n - *d; *d = 0; SP_GC_CTR_SUB(sp_gc_bytes, rem); }
}
#else
static inline void sp_gc_bytes_add(size_t n) { sp_gc_bytes += n; }
/* Floored: the counter is a heuristic the retune divides by, and a wrapped
   value there is not a large heap but a collector that never triggers again
   (#4073). */
/* Floored. The counter is a heuristic the retune divides by and multiplies, and
   an array-growth path can subtract a capacity larger than the counter holds
   (measured: sp_array.h's StrArray grow, 20472 against 10344) -- wrapping it
   makes the trigger fire on every allocation until the next collection resets
   it, and used to make the retune set a threshold that never fires at all. */
static inline void sp_gc_bytes_sub(size_t n) {
  sp_gc_bytes = sp_gc_bytes >= n ? sp_gc_bytes - n : 0;
}
#endif

/* Push a header onto the shared sp_gc_heap list. Under SP_THREADS this is a
   lock-free CAS push so callers that hold no lock (the pool-hit relink) stay
   off the heap mutex; the allocators, which hold the mutex anyway for the
   collect trigger, use the same push so every writer to the list head agrees
   on one protocol (a plain locked store racing an unlocked CAS would itself
   be a race). The `next` store is atomic: a node being re-linked from a
   shared pool free list may still have a stale pool popper reading its
   `next` (that popper's CAS then fails and discards the value, but the read
   itself must be defined). Removals happen only in the stop-the-world sweep
   with every mutator parked, so the push never races a pop and needs no ABA
   defense. Release order publishes the node's initialized header to the
   collector. */
/* The slab's reservation (lib/sp_slab.c): a block is the slab's when its
   address falls inside it, which the allocation fast paths test inline. */
extern uintptr_t sp_slab_base;
extern size_t sp_slab_cap;
static inline int sp_slab_owns(const void *p) { return (uintptr_t)p - sp_slab_base < sp_slab_cap; }
/* Only a block the slab does not hold goes on a list: a slab block's
   generation is a bit in its chunk's bitmaps, set by the allocation itself
   (lib/sp_slab.c), and the lists carry what fell back to malloc. */
#ifdef SP_THREADS
/* Per-worker young list: only this worker's M pushes here (started threads are
   pinned and a worker pumps one green thread at a time), so a plain store is
   race-free -- no CAS, no shared-head cache-line bounce. */
#define SP_GC_HEAP_PUSH(hdr) do { if (!sp_slab_owns(hdr)) { \
    sp_gc_hdr **_sp_head = &sp_gc_wslot[sp_worker_id].young; \
    (hdr)->next = *_sp_head; *_sp_head = (hdr); } \
  } while (0)
#else
#define SP_GC_HEAP_PUSH(hdr) do { if (!sp_slab_owns(hdr)) { (hdr)->next = sp_gc_heap; sp_gc_heap = (hdr); } } while (0)
#endif

/* ---- Slab allocator for GC objects and heap strings (lib/sp_slab.c) ----
 * sp_slab_alloc answers a zeroed block of `need` bytes (header included) from
 * the calling worker's size-class chunks, in the current epoch's young
 * generation; sp_slab_alloc_str the same for a heap string (unzeroed, its
 * bytes counted on the string side); sp_slab_alloc_raw a payload no sweep
 * frees. A block's generation, mark and finalizer bit live in its chunk's
 * bitmaps, which is what the sweep reads (sp_slab_sweep_worker) and the
 * mark sets (sp_slab_mark). A size past the largest class, or the allocator
 * off (SPINEL_GC_SLAB=0), falls back to calloc/malloc, and such a block goes
 * on the young/old LISTS instead; sp_slab_owns tells the two apart by
 * address: every chunk lives inside one reserved range. sp_slab_release, at
 * the end of a full cycle, returns fully free chunks to the OS. */
void *sp_slab_alloc(size_t need);        /* an object: zeroed, in the current epoch's young generation */
void *sp_slab_alloc_obj(size_t need, void (*fin)(void *), void (*scn)(void *));   /* the same, with the header written */
void *sp_slab_alloc_str(size_t need);    /* a heap string: young, its bytes counted on the string side */
void *sp_slab_alloc_raw(size_t need);    /* a payload: no sweep frees it, only sp_slab_free */
void  sp_slab_free(void *p);
void  sp_slab_set_fin(void *hdr);        /* the object gained a finalizer or recycler after allocation */
void  sp_slab_park(void *hdr);           /* a dead header about to be pooled: out of every generation's reach */
void  sp_slab_relive(void *hdr);         /* a pooled header handed out again */
void  sp_slab_pin(const void *p);        /* a frozen heap string: immortal, as a literal is */
int   sp_slab_is_str(const void *p);
int   sp_slab_is_live(const void *p);    /* allocated, in some generation */
int   sp_slab_is_old(const void *p);
int   sp_slab_is_marked(const void *p);
void  sp_slab_describe(const void *p);   /* the verifiers: what the bitmaps say about a slot */
void  sp_slab_verify_all(void);          /* SPINEL_GC_VERIFY: the bitmaps against their invariants */
void  sp_slab_unmark(const void *p);
/* the mark: sets the slot's mark bit, promotes a young slot (unless `aging`
   keeps it young); 0 when the slot was already marked this cycle */
int   sp_slab_mark(const void *p, int aging, int *was_young);
extern unsigned sp_slab_epoch;
extern SP_TLS unsigned long sp_slab_frees;   /* this thread's explicit frees, counted */
void  sp_slab_epoch_flip(void);          /* under the barrier: new allocations go to the other parity */
void  sp_slab_runs_release(void);
void  sp_slab_history(const void *p);    /* SPINEL_GC_VERIFY: print a slot's recorded events */
extern int sp_gc_alloc_fast_ok;         /* sp_gc_alloc's lean front may run: drop to 0 to route every allocation through the full form */        /* under the barrier: the workers' claimed-not-allocated runs are unclaimed */
typedef struct { size_t freed_obj, freed_str, freed_slots, slots, swept, kept_young, parked; } sp_slab_sweep_stats;   /* swept: finalizers run; parked: bytes of headers held by their pools */
/* one worker's chunks: frees what the closed epoch holds unmarked (and,
   at a full cycle, the old generation's unmarked); `die` runs a dead
   object's finalizer or recycler and answers 1 when the slot is freed, 0
   when the pool keeps the header */
void  sp_slab_sweep_worker(int wid, int full, int aging, int (*die)(void *hdr), sp_slab_sweep_stats *st);
void  sp_slab_each_object(int young, int old, void (*fn)(void *hdr, void *arg), void *arg);
void  sp_slab_each_string(int young, int old, void (*fn)(void *hdr, void *arg), void *arg);
void  sp_slab_release(void);
void  sp_slab_release_worker(int wid);   /* one worker's lists, by their owner, beside the program */
void  sp_slab_release_from(int first);   /* the slots from `first` on, under the barrier */
extern int sp_slab_on;

/* ---- Collector entry points (defined in lib/sp_gc.c) ---- */
int  sp_gc_verify_on(void);   /* SPINEL_GC_VERIFY is set (diagnostics only) */
extern const char *sp_gc_dbg_phase;   /* which root group the mark walk is in */
extern void *sp_gc_dbg_ctx;
void sp_gc_mark(void *obj);
void sp_gc_mark_all(void);
void sp_gc_mark_drain(void);
extern int sp_gc_minor;
extern int sp_gc_young_probe_on, sp_gc_young_probe_hit;
extern int sp_gc_age_survivors, sp_gc_age_on, sp_gc_root_phase;
extern size_t sp_gc_old_live;
extern size_t sp_gc_young_kept_bytes, sp_gc_npromoted;
extern int sp_gc_minor_on;
extern int sp_gc_verify_gen;
extern int sp_gc_verify_gen_fail;
extern int sp_gc_verify_probe_on, sp_gc_verify_probe_hit;
extern unsigned sp_gc_verify_probe;
/* Per-phase collector time, in seconds, cumulative (SPINEL_GC_PHASES=1; all
   zero when it is off). sp_gc_stat_seconds is their sum plus the bookkeeping
   between them. Reported by sp_alloc.c, which is where the stats line lives. */
/* Objects marked and slots swept since the process started. Counts are what
   the two phases' costs are actually per; see sp_gc_sweep_young. */
extern size_t sp_gc_ct_swept, sp_gc_ct_marked;
extern double sp_gc_ph_mark, sp_gc_ph_oldsweep, sp_gc_ph_slotsweep,
              sp_gc_ph_rembclear, sp_gc_ph_strsweep, sp_gc_ph_trim;
/* The mark, split the way sp_gc_mark_all walks: this worker's own root stack,
   every live fiber's saved roots, the globals hook, then the trace that drains
   what those three found. Named as sp_gc_dbg_phase names them under verify, so
   a number leads to the code. They sum to sp_gc_ph_mark.

   The split exists because "mark grew" has two causes that need different
   answers: more ROOTS to scan (the fibers row, which grows with in-flight
   fibers) and more GRAPH to trace (the scan row, which grows because those
   fibers hold live objects). Only the first is what slicing the fiber list to
   the parked workers would address (#4384). */
extern double sp_gc_ph_slot_max;   /* longest single sweep task, summed (sp_sched.c) */
extern double sp_gc_ph_task_sum, sp_gc_ph_task_obj, sp_gc_ph_task_sold, sp_gc_ph_task_syoung;   /* all tasks' time, by kind */
extern unsigned long long sp_gc_ph_mk_helpers, sp_gc_ph_mk_drains;
extern double sp_gc_ph_mk_drain, sp_gc_ph_mk_join, sp_gc_ph_mk_idle;
extern double sp_gc_ph_conc_wait, sp_gc_ph_conc_wall, sp_gc_ph_barrier, sp_gc_ph_park, sp_gc_ph_apply_obj, sp_gc_ph_apply_str, sp_gc_ph_apply_release; extern unsigned long long sp_gc_ph_conc_waits;
extern double sp_gc_ph_mk_roots, sp_gc_ph_mk_fibers,
              sp_gc_ph_mk_globals, sp_gc_ph_mk_scan;
extern int sp_gc_ph_on;
void sp_gc_collect(void);
#ifdef SP_THREADS
/* Sweep one worker's young list on that worker (see sp_gc.c). Survivors come
   back as a local list for the collector to splice into the old heap. */
void sp_gc_sweep_slot(int wid, sp_gc_hdr **out_head, sp_gc_hdr **out_tail, size_t *out_bytes);
void sp_gc_sweep_list(sp_gc_hdr **pp, int conc, sp_gc_hdr **out_head, sp_gc_hdr **out_tail, size_t *out_bytes);
void sp_gc_sweep_old_list(sp_gc_hdr **pp, size_t *out_live, sp_gc_hdr **out_tail);
extern void (*sp_gc_par_sweep_hook)(void);
/* The parallel mark: the scheduler lends parked workers to the drain. */
extern void (*sp_gc_par_mark_hook)(void);
void sp_gc_mark_par_run(void);
void sp_gc_mark_par_begin(void);
void sp_gc_mark_par_markers(int n);
extern int sp_gc_par_mark_on;
/* The header's flag word: `marked` (27 bits) and the five bits after it,
   which the C bit-fields lay out from the low end of one 32-bit unit on
   every target spinel runs on. The parallel mark claims a stamp with an
   exchange on this word; sp_gc_hdr_flags_check aborts at start-up if the
   compiler laid the fields out differently. */
#define SP_GC_FL_MARK_MASK 0x07ffffffu
#define SP_GC_FL_FROZEN    (1u << 27)
#define SP_GC_FL_PINNED    (1u << 28)
#define SP_GC_FL_OLD       (1u << 29)
#define SP_GC_FL_DIRTY     (1u << 30)
#define SP_GC_FL_AGED      (1u << 31)
static inline unsigned *sp_gc_hdr_flags(sp_gc_hdr *h) { return (unsigned *)((char *)h + offsetof(sp_gc_hdr, size) + sizeof(size_t)); }
void sp_gc_hdr_flags_check(void);
/* The concurrent sweep (see sp_gc.c): the driver installs both or neither. */
extern void (*sp_gc_conc_sweep_hook)(int full, int str_sweep, int str_major);
extern void (*sp_gc_conc_wait_hook)(void);
extern int sp_gc_conc_on;
/* The old object list, for the driver: detach it whole for a full cycle's
   sweep, attach a list (survivors, or the swept remainder) back. */
void sp_gc_sweep_chunks(int wid, int full);   /* one slot's slab chunks, by their bitmaps (lib/sp_gc.c) */
sp_gc_hdr *sp_gc_old_detach(void);
void sp_gc_old_attach(sp_gc_hdr *head, sp_gc_hdr *tail);
/* Splice one worker's survivors onto the shared old heap. Collector-only. */
void sp_gc_promote_slot(sp_gc_hdr *head, sp_gc_hdr *tail, size_t bytes);
#endif
void sp_gc_enforce_mem_limit(void);
extern int sp_gc_trim_wanted;   /* set by a full cycle; the trimmer thread clears it with a malloc_trim */
extern int sp_gc_trimmer_on;
/* Collect + re-tune the threshold, assuming exclusive heap access (see
   sp_alloc.c). sp_stw_collect (sp_sched.c, threaded build) stops the world then
   runs sp_gc_collect_retune; the single-threaded allocator calls it directly
   under the heap lock. */
void sp_gc_collect_retune(void);
void sp_stw_collect(void);
void sp_gc_collect_request(void);   /* explicit GC.start: same barrier, forced */
/* ObjectSpace finalizers left pending by a collection. They run at a safe
   point of the program -- a method's entry, a loop's back-edge, GC.start,
   exit -- never inside the allocation that collected, where the runtime may
   be halfway through an operation on an object the finalizer can reach. The
   generated code polls the flag only when the program defines finalizers. */
extern volatile int sp_fin_pending_flag;
void sp_fin_run_pending(void);
#define SP_FIN_POLL() \
  do { if (SP_UNLIKELY(SP_ATOMIC_LOAD(&sp_fin_pending_flag, __ATOMIC_RELAXED))) sp_fin_run_pending(); } while (0)
void sp_oom_die(void);

/* ---- Embedder callbacks supplied by the generated TU ----
 * The collector cannot own the program's roots or string heap (they are
 * static state in the generated TU: the regexp match globals, ARGV, the
 * in-flight exception stack, and the heap-string free list). The TU
 * installs its mark-roots and string-sweep callbacks here at startup;
 * sp_gc_mark_all / sp_gc_collect invoke them through these pointers, the
 * same way fibers register sp_gc_mark_suspended_fibers_hook. */
extern void (*sp_gc_mark_globals_hook)(void);
extern void (*sp_gc_str_sweep_hook)(void);
/* Whether the string heap's own schedule (or its growth backstop) would take
   a major this cycle. A string major needs a whole-heap mark, so under the
   minor mark the object cycle it lands on has to be full; the collector asks
   this before it decides. */
extern int (*sp_gc_str_major_due_hook)(void);

/* ---- value-introspection hooks (set by the generated TU at startup) ----
 * lib/sp_json.c (and other cold readers) own no container types; they reach the
 * generated TU's typed arrays/hashes only through these generic readers, the
 * same idiom as the GC hooks above. sp_sym_name maps a symbol id to its name;
 * sp_json_kind classifies a boxed value (1=array, 2=hash, 0=other); len/aref
 * iterate any array; hpair yields a hash's (key,value) at insertion index i. */
extern const char *(*sp_sym_name_fn)(sp_sym);
extern int (*sp_json_kind_fn)(sp_RbVal);
extern sp_int (*sp_json_len_fn)(sp_RbVal);
extern sp_RbVal (*sp_json_aref_fn)(sp_RbVal, sp_int);
extern void (*sp_json_hpair_fn)(sp_RbVal, sp_int, sp_RbVal *, sp_RbVal *);
/* Container BUILDERS for JSON.parse (installed by the generated TU, which owns
   the hash type): make an empty string-keyed hash, and set a (key, value) pair
   -- CRuby's JSON.parse returns String keys. Arrays are built directly from the
   package ABI (sp_PolyArray). */
extern sp_RbVal (*sp_json_mk_hash_fn)(void);
extern sp_sym (*sp_json_sym_intern_fn)(const char *);  /* symbolize_names key interner (TU-installed) */
extern void (*sp_json_hash_set_fn)(sp_RbVal, const char *, sp_RbVal);
/* Recursive #inspect of a boxed value, for lib/sp_inspect.c's container walker
   (set to sp_poly_inspect; same idiom as the JSON hooks). */
extern const char *(*sp_poly_inspect_fn)(sp_RbVal);
/* #to_s of any boxed value, for a package that writes one (StringIO#puts):
   the generated TU installs sp_poly_to_s, which renders a user object
   through its own #to_s or the #<Name:0xADDR> default. */
extern const char *(*sp_poly_to_s_fn)(sp_RbVal);
/* Convert a plain object (a Struct) to a boxed StrPoly hash of its members,
   generic (no format knowledge). The generated program installs it (switch on
   cls_id) when it has Structs and a package consumes it; a consumer such as
   the json package reads it to serialize an object as a hash. NULL otherwise. */
extern sp_RbVal (*sp_obj_to_hash_fn)(sp_RbVal);
extern const char *(*sp_obj_to_json_fn)(sp_RbVal);
/* Symbol-keyed Struct/Data #to_h, for a poly receiver (#2906). */
extern sp_RbVal (*sp_obj_to_h_fn)(sp_RbVal);
extern sp_RbVal (*sp_obj_struct_values_fn)(sp_RbVal);
/* user-object #to_a for container-read poly receivers (#3234): installed by
   the generated prologue when any instantiated class defines a no-arg to_a */
extern sp_RbVal (*sp_obj_to_a_fn)(sp_RbVal);
/* #to_ary, the CONVERSION protocol -- distinct from to_a, the enumeration
   one: Kernel#Array asks to_ary first, and only a class that defines it
   answers here (#4187). */
extern sp_RbVal (*sp_obj_to_ary_fn)(sp_RbVal);
/* The array a `case/in` array pattern matches a user object against. Separate
   from #to_a: a Data answers #deconstruct but has no #to_a at all. */
extern sp_RbVal (*sp_obj_deconstruct_fn)(sp_RbVal);
/* 1 when cls_id is a Data class. Data defines no #dig, so a dig that lands on
   one is the TypeError CRuby raises rather than a member read; Struct, which
   does define #dig, is unaffected. */
extern int (*sp_obj_is_data_fn)(int cls_id);
/* Data#with copy-update for a poly receiver: (value, symbol-keyed overrides). (#2890) */
extern sp_RbVal (*sp_obj_with_fn)(sp_RbVal, sp_RbVal);
/* default Object#inspect for user objects: the generated TU installs a
   per-class ivar walk (sp_obj_inspect_sw); sp_poly_inspect's OBJ default
   consults it so nested/boxed objects render like CRuby */
extern const char *(*sp_obj_inspect_fn)(int cls_id, void *p);
/* Same shape for user #to_s: sp_poly_to_s's OBJ default consults it so a
   boxed user object with a custom to_s renders through it. */
extern const char *(*sp_obj_to_s_fn)(int cls_id, void *p);
/* CRuby's implicit conversion protocol on BOXED user objects: the runtime's
   Integer/String conversion sites (pack, typed-slot coercions) reach a
   compiled #to_int / #to_str through these. A class without the method is
   the default arm (*ok = 0 / NULL) and the caller raises CRuby's TypeError. */
extern sp_int (*sp_obj_to_int_fn)(int cls_id, void *p, int *ok);
extern const char *(*sp_obj_to_str_fn)(int cls_id, void *p);
/* #to_path, which File/Dir/IO's path slots ask before #to_str (CRuby's
   rb_get_path); NULL when the class defines none. */
extern const char *(*sp_obj_to_path_fn)(int cls_id, void *p);
/* Kernel#Integer / Kernel#Float on a boxed user object: the class's #to_int,
   #to_i, #to_f or #to_str (`which`, in that order; 4 is #to_f for a Math
   argument, 5 #to_hash for a `**` operand), WHATEVER the method's
   static type -- CRuby calls it and judges the answer -- boxed into *out.
   Answers 1 when the class has the method, 0 when it does not; with a NULL
   `out` it only answers that, calling nothing. */
extern int (*sp_obj_conv_fn)(int cls_id, void *p, int which, sp_RbVal *out);
/* Ruby class name for a user cls_id (the generated id->name table), so a
   runtime TU can word a TypeError the way CRuby does. */
extern const char *(*sp_obj_cls_name_fn)(int cls_id);
/* Is user class `sub` the class `super` or a descendant of it? The generated
   class bank installs it; NULL means only an exact id can be trusted. A
   pointer array of one class checks a stored object against it (#4486). */
extern int (*sp_class_le_id_fn)(int sub, int super);
/* Module#<=> of two boxed classes (-1/0/1, nil when unrelated), by the class
   graph the generated program installs. */
extern sp_RbVal (*sp_class_cmp_fn)(sp_RbVal a, sp_RbVal b);
/* the same walk from a user class id, matching the ancestor by the NAME the
   runtime reader holds (sp_class_kind_of_name in the generated program) */
extern int (*sp_class_kind_of_name_fn)(int cls, const char *name);
/* is a class value a module? (sp_class_is_module_val in the generated
   program; NULL leaves only the builtin modules known) */
extern int (*sp_class_is_module_fn)(sp_Class c);

/* ---- Hot inline mark helpers (inlined into both sides) ----
 * String tag bytes: 0xfe heap-unmarked -> 0xfc marked (0xfa -> 0xf8 for a
 * collectable frozen one); others skipped. */
void sp_gc_mark_str(const char *s);   /* lib/sp_gc.c: the bitmap mark of a slab string, the byte of a malloc'd one */
static inline void sp_mark_string(const char *s) {
  if (!s) return;
  /* 0xfe or 0xfa in one compare: a second branch in this layout-sensitive
     inline costs optcarrot's mark (#1449) */
  if (((unsigned char)s[-1] | 0x04) == 0xfe) {
    sp_gc_mark_str(s);
    return;
  }
  /* 0xfd is a mutable String's payload, whose lifetime belongs to the handle
     in front of it: marking the bytes alone leaves the handle unreferenced,
     and its finalizer frees the bytes this container still points at. The
     payload's `next` field carries that handle (sp_fd_own). */
  if ((unsigned char)s[-1] == 0xfd) {
    void *owner = (void *)(((const sp_str_hdr *)(s - 1)) - 1)->next;
    if (owner) sp_gc_mark(owner);
  }
  /* No frozen (0xf1) branch here: this is inlined into optcarrot's GC mark and
     is layout-sensitive. A live frozen heap string is kept immortal by
     sp_str_sweep instead (#1449). */
}
static inline void sp_mark_rbval(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) sp_mark_string(v.v.s);
  /* A class boxed by name holds its name: `e.class` of an exception names it
     with a fresh copy (sp_exc_class_name), a Method's owner likewise, and
     nothing else keeps that copy -- an Array holding the class printed the
     next string allocated in its place. Literal names (SPL) are not heap
     strings and mark as nothing. */
  else if (v.tag == SP_TAG_CLASS && v.cls_id == SP_CLASS_BY_NAME) sp_mark_string(v.v.s);
  else if (v.tag == SP_TAG_OBJ && v.cls_id != SP_BUILTIN_FOREIGN_PTR &&
           v.cls_id != SP_BUILTIN_REGEX && v.cls_id != SP_BUILTIN_ARGF) sp_gc_mark(v.v.p);
  else if (v.tag == SP_TAG_BIGINT) sp_gc_mark(v.v.p);
}
/* A scratch root: the proc calling convention's side channel keeps its last
   value after the value's reader is done with it, so by the next collection
   it may name a slot a sweep has freed since. Marking that would set
   generation bits on a free slot and run whatever scan hook its stale bytes
   hold. A slab slot that is free now is skipped; one that was reused is
   some live object, marked a cycle longer than needed, which is harmless. */
static inline void sp_mark_rbval_scratch(sp_RbVal v) {
  const void *h = NULL;
  if (v.tag == SP_TAG_STR || (v.tag == SP_TAG_CLASS && v.cls_id == SP_CLASS_BY_NAME)) { if (v.v.s && ((unsigned char)v.v.s[-1] | 0x04) == 0xfe) h = ((const sp_str_hdr *)(v.v.s - 1)) - 1; }
  else if ((v.tag == SP_TAG_OBJ && v.cls_id != SP_BUILTIN_FOREIGN_PTR && v.cls_id != SP_BUILTIN_REGEX && v.cls_id != SP_BUILTIN_ARGF) || v.tag == SP_TAG_BIGINT) { if (v.v.p) h = (const char *)v.v.p - sizeof(sp_gc_hdr); }
  if (h && sp_slab_owns(h) && !sp_slab_is_live(h)) return;
  sp_mark_rbval(v);
}
/* Closure-cell content markers. A captured non-int local is laundered into the
   pointer-sized sp_int cell as (uintptr_t)<ptr>; the cell's GC scan marks the
   referent so it survives as long as the capturing proc does. */
static inline void sp_cell_scan_str(void *p) { sp_mark_string(*(const char **)p); }
static inline void sp_cell_scan_ptr(void *p) { sp_gc_mark(*(void **)p); }
static inline void sp_cell_scan_rbval(void *p) { sp_mark_rbval(*(sp_RbVal *)p); }
/* A captured String range rides a cell of its own struct: both endpoints are
   strings to mark (sp_box_srange's scan, for the cell). */
static inline void sp_cell_scan_srange(void *p) {
  sp_StrRange *r = (sp_StrRange *)p;
  if (r->first) sp_mark_string(r->first);
  if (r->last) sp_mark_string(r->last);
}
/* A captured Proc rides in an sp_int cell as (sp_int)(uintptr_t)ptr -- the cell
   is an integer slot, but what it holds is a collectable object, and without a
   scan the capture kept the CELL alive and nothing kept the proc. A nested
   `proc { |v| two.call(v, v) }` then called through freed memory (#4077). */
static inline void sp_cell_scan_procint(void *p) {
  sp_int v = *(sp_int *)p;
  if (v) sp_gc_mark((void *)(uintptr_t)v);
}
/* A low-bit-tagged root entry is an sp_RbVal* (see SP_GC_ROOT_RBVAL);
   an untagged entry is a plain void** to a direct GC pointer. */
static inline void sp_gc_mark_root_entry_flat(void **e) {
  uintptr_t u = (uintptr_t)e;
  if (u & (uintptr_t)1) { sp_mark_rbval(*(sp_RbVal *)(u & ~(uintptr_t)1)); }
  else if (u & (uintptr_t)2) { sp_mark_string(*(const char **)(u & ~(uintptr_t)2)); }
  else { void *o = *e; if (o) sp_gc_mark(o); }
}
/* A root frame (tag 3, see SP_GC_ROOT_FRAME): the boxed slots first, then the
   tagged entries, each of which is a per-root entry relocated into the frame
   -- never another frame. An entry the function has not reached yet is still
   the zero the frame started with, and is skipped. */
static inline void sp_gc_mark_frame(const sp_gc_frame_hdr *h) {
  const sp_RbVal *v = (const sp_RbVal *)(h + 1);
  for (int i = 0; i < h->nv; i++) sp_mark_rbval(v[i]);
  void **const *p = (void **const *)(v + h->nv);
  for (int i = 0; i < h->np; i++) if (p[i]) sp_gc_mark_root_entry_flat(p[i]);
}
static inline void sp_gc_mark_root_entry(void **e) {
  uintptr_t u = (uintptr_t)e;
  if ((u & (uintptr_t)3) == (uintptr_t)3) sp_gc_mark_frame((const sp_gc_frame_hdr *)(u & ~(uintptr_t)3));
  else sp_gc_mark_root_entry_flat(e);
}

#endif
