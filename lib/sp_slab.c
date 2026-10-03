/* sp_slab.c -- size-class slab allocation for GC objects and heap strings,
 * with the generations and the mark kept in bitmaps beside the chunks.
 *
 * Every object and every heap string used to be one malloc and, when it
 * died, one free. On a server that allocates nine thousand of them per
 * request from thirty workers, that was 40% of the CPU: glibc's arena locks
 * and consolidation on the way in, and a sweep that handed every dead block
 * back through free() on the way out, cold, from whichever thread ran the
 * collection. Nothing a faster general allocator fixes -- tcmalloc measured
 * the same -- because the cost is the per-block call and the cross-thread
 * free, not the arithmetic.
 *
 * Here a block is a slot in a 16 KB chunk of one size class, and what the
 * collector knows about a slot lives in seven bitmaps kept in the arena's
 * metadata chunks, one bit per slot each, never in the slot itself:
 *
 *   young[0], young[1]  allocated in the epoch of that parity (see below)
 *   old                 promoted
 *   mark                reached by this cycle's mark
 *   fin                 an object whose death runs a finalizer or recycler
 *   str                 a heap string (its bytes count on the string side)
 *   pin                 never freed by a sweep: a frozen heap string, a
 *                       container payload, a dead header parked on its
 *                       pool (the last two are in no generation at all and
 *                       die by an explicit free, or come back by a relive)
 *
 * A slot is free when it is in no generation and not pinned (young[0],
 * young[1], old, pin all clear), and allocation is a find-first-zero over
 * those four words and one or into the current epoch's young word (atomic
 * when threaded; single-threaded the words are plain). That replaced the
 * per-chunk free list linked through the blocks, and the reason is the
 * sweep: with the list, a sweep touched the header of every dead object to
 * unlink it and to thread the free list through it, and on a server that
 * churns a hundred megabytes of small blocks between two collections that
 * was a stream of cache misses worth a fifth of the process. A sweep here
 * reads the bitmaps of a chunk -- a few cache lines for sixteen kilobytes of
 * slots -- and never the slots, except the dead ones whose `fin` bit says a
 * finalizer has to run.
 *
 * The EPOCH is what tells the generation a concurrent sweep is reclaiming
 * from what the running program has allocated since the barrier: the
 * collector flips the parity under the barrier, so everything allocated
 * from then on goes into the other young bitmap, and the sweep frees from
 * the one that stopped growing. The mark promotes what it reaches -- it
 * sets `old` on a young survivor, as the header's `old` bit is set there
 * too -- so after the mark the swept young word is exactly the dead plus
 * the promoted, and clearing it whole is the sweep's write.
 *
 * Every chunk lives inside one address range reserved at startup (16 GB of
 * untouched, uncommitted pages; smaller where the system refuses), so a free
 * tells a slab block from a malloc'd one by a range check, and a slot finds
 * its chunk by masking its address. The range is handed out in 4 MB arenas
 * whose first chunk holds the arena's chunk headers and whose next seven
 * hold the bitmaps, so a released chunk keeps no resident page at all. At
 * the end of a full cycle every chunk that is entirely free, beyond a small
 * reserve per worker, is given back to the OS with madvise and re-carved
 * from scratch when next used; that is what malloc_trim did for these
 * blocks, at a syscall per idle chunk instead of a walk of thirty arenas.
 *
 * A chunk belongs to one worker for allocation and no other worker
 * allocates from it, and every chunk a worker owns is on its owned list,
 * which is what its sweep walks. Frees come from anywhere -- a sweeper
 * thread, a finalizer on another worker, the explicit free of a payload --
 * and are atomic bit clears; the one structure they share with the owner
 * beyond the bitmaps is the owner's available list, pushed with the same
 * compare-and-swap the owner pops with.
 *
 * SPINEL_GC_SLAB=0 turns this off and every block is a malloc again, which
 * is the configuration ASAN wants: a slab hides a use-after-free from it.
 * It is on by default whatever the process's malloc (it used to default off
 * under jemalloc; see sp_slab_jemalloc_present for why that changed). */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include "sp_gc.h"
#include "sp_alloc.h"   /* sp_gc_alloc is here: the collector's bookkeeping around the claim */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define SP_SLAB_ARENA   ((size_t)4 << 20)
#define SP_SLAB_CHUNK   ((size_t)16 << 10)
#define SP_SLAB_NCHUNK  (SP_SLAB_ARENA / SP_SLAB_CHUNK)   /* 256 */
#define SP_SLAB_NCLS    27
#define SP_SLAB_MAX     2048
/* chunk 0 is the header table (256 x 64 B); chunks 1..SP_SLAB_META hold the
   bitmaps (256 x 448 B = 112 KB = 7 chunks); the rest are carved */
#define SP_SLAB_META    7
#define SP_SLAB_FIRST   (1 + SP_SLAB_META)
/* fully free chunks a worker keeps resident across a full cycle per class, at least (64 KB) */
#define SP_SLAB_RESERVE 4
/* the most slots a chunk holds (16 KB / 32 B), in 64-bit words */
#define SP_SLAB_NW      8

#ifdef SP_THREADS
#define SP_SLAB_NWK SP_MAX_WORKERS
#define SP_SLAB_WID() (sp_worker_id)
static inline int sp_worker_id_shadow(void) { return sp_worker_id; }
#else
#define SP_SLAB_NWK 1
#define SP_SLAB_WID() 0
static inline int sp_worker_id_shadow(void) { return 0; }
#endif
void sp_slab_note(const void *p, int what);
extern int sp_slab_verify_on;

/* 16-byte steps to 256, then coarser: an object header is 48 bytes and a
   string header 24, and most blocks are a header plus a few words, so the
   fine steps are where the population is. Past SP_SLAB_MAX a block is
   malloc'd as before. */
static const uint16_t sp_slab_csize[SP_SLAB_NCLS] = {
  32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256,
  320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048 };
static uint8_t sp_slab_cls_of[SP_SLAB_MAX / 16 + 1];   /* (need+15)/16 -> class */
/* offset-in-chunk / csize as a multiply: (off * recip) >> 32 is exact for
   every off < 16 KB, checked at init */
static uint32_t sp_slab_recip[SP_SLAB_NCLS];
static uint16_t sp_slab_nslots_of[SP_SLAB_NCLS];

typedef struct sp_slab_chunk {
  struct sp_slab_chunk *next_avail;   /* the owner's available list, per class */
  struct sp_slab_chunk *own_next, *own_prev;   /* the owner's owned list, every chunk it carved */
  uint32_t nslots;
  uint16_t hint;                      /* the word the last allocation came from */
  uint16_t cls, wid;
  uint8_t on_avail;                   /* listed on the owner's available list */
  uint8_t in_use;                     /* holds a class; 0 = empty, on the global pool */
  uint8_t touched;                    /* has resident pages since the last release */
  uint8_t _pad[64 - sizeof(void *) * 3 - 4 - 2 * 3 - 3];   /* a 64-byte header on any pointer width */
} sp_slab_chunk;
typedef char sp_slab_chunk_is_one_line[sizeof(sp_slab_chunk) == 64 ? 1 : -1];

/* one slot's worth of state per chunk: seven bitmaps of SP_SLAB_NW words */
typedef struct sp_slab_bm {
  uint64_t young[2][SP_SLAB_NW];
  uint64_t old[SP_SLAB_NW];
  uint64_t mark[SP_SLAB_NW];
  uint64_t fin[SP_SLAB_NW];
  uint64_t str[SP_SLAB_NW];
  uint64_t pin[SP_SLAB_NW];
} sp_slab_bm;
typedef char sp_slab_bm_is_448[sizeof(sp_slab_bm) == 448 ? 1 : -1];
typedef char sp_slab_meta_fits[SP_SLAB_NCHUNK * sizeof(sp_slab_bm) <= SP_SLAB_META * SP_SLAB_CHUNK ? 1 : -1];

typedef struct sp_slab_arena {
  sp_slab_chunk ch[SP_SLAB_NCHUNK];   /* ch[0] is this table itself, never carved */
} sp_slab_arena;

typedef struct {
  sp_slab_chunk *cur[SP_SLAB_NCLS];
  sp_slab_chunk *avail[SP_SLAB_NCLS];
  /* The allocation cache, per class: a word of the current chunk with its
     free bits (wmask, not claimed yet), and per KIND (0: objects and
     strings, claimed in the epoch's young word; 1: payloads, claimed in
     pin) a RUN of consecutive slots cut from that word, claimed in the
     bitmap all at once and handed out by a bump of rnext up to rend. The
     two kinds keep their runs through each other's refills: dropping a
     run on a kind switch orphaned its claimed rest until the next
     collection, and a server alternating hash tables with objects of the
     same class orphaned gigabytes that way. A slot claimed and not yet
     handed out is in the bitmap and garbage in memory; nothing reads it,
     and the collector unclaims every worker's remainders under the barrier
     (sp_slab_runs_release) before it walks or sweeps anything. */
  uint64_t wmask[SP_SLAB_NCLS];     /* free bits of the cached word not claimed yet */
  uint64_t *wyoung[SP_SLAB_NCLS];   /* the cached word in this epoch's young bitmap */
  uint64_t *wpin[SP_SLAB_NCLS];     /* the same word of the pin bitmap */
  uint64_t *wfin[SP_SLAB_NCLS];     /* the same word of the fin bitmap */
  uint64_t *wstr[SP_SLAB_NCLS];     /* the same word of the str bitmap */
  char *wbase[SP_SLAB_NCLS];        /* slot 0 of that word */
  char *rnext[2][SP_SLAB_NCLS];     /* the run's next slot, per kind */
  char *rend[2][SP_SLAB_NCLS];      /* one past the run's last slot */
  char *rbase[2][SP_SLAB_NCLS];     /* slot 0 of the run's word (the fin bit is computed from it) */
  uint64_t *rclaim[2][SP_SLAB_NCLS];   /* the word the run was claimed in (unclaimed from there at a release) */
  uint64_t *rfin[2][SP_SLAB_NCLS];     /* the run's word in the fin bitmap */
  uint64_t *rstr[2][SP_SLAB_NCLS];     /* the run's word in the str bitmap */
  sp_slab_chunk *owned;              /* every chunk this worker carved, doubly linked */
  long taken;        /* chunks this worker started allocating into since the last release */
  int taken_cls[SP_SLAB_NCLS];   /* the same, per class: what each class will need again next cycle */
  int sweeping;      /* a sweep of this worker's chunks is running (the release waits it out) */
  char _pad[64 - ((19 * SP_SLAB_NCLS * sizeof(void *) + SP_SLAB_NCLS * (sizeof(uint64_t) + sizeof(int)) + sizeof(void *) + sizeof(long) + sizeof(int)) % 64)];
} sp_slab_worker;

static sp_slab_worker sp_slab_wk[SP_SLAB_NWK];
uintptr_t sp_slab_base = 0;   /* the reservation (read inline by sp_slab_owns) */
static uintptr_t sp_slab_brk = 0;   /* how much of it is in use */
size_t sp_slab_cap = 0;
static sp_slab_chunk *sp_slab_empty = NULL;   /* chunks holding no class */
int sp_slab_on = -1;                          /* decided once from the environment */
/* The parity of the epoch new allocations join. The collector flips it under
   the barrier (sp_slab_epoch_flip), where no mutator runs. */
unsigned sp_slab_epoch = 0;
#ifdef SP_THREADS
#include <pthread.h>
static pthread_mutex_t sp_slab_lock = PTHREAD_MUTEX_INITIALIZER;
#define SP_SLAB_LOCK()   pthread_mutex_lock(&sp_slab_lock)
#define SP_SLAB_UNLOCK() pthread_mutex_unlock(&sp_slab_lock)
#else
#define SP_SLAB_LOCK()   ((void)0)
#define SP_SLAB_UNLOCK() ((void)0)
#endif
/* The bitmap operations. Threaded, every one is atomic: a sweeper clears
   bits of a chunk its owner allocates from, a pool hands a header back on
   another thread, a finalizer frees a payload from anywhere. Single-
   threaded nothing runs beside the program, and a locked instruction per
   allocation and per death was a fifth of an allocation-bound benchmark. */
#ifdef SP_THREADS
static inline uint64_t bm_or(uint64_t *p, uint64_t v) { return SP_ATOMIC_FETCH_OR(p, v, __ATOMIC_ACQ_REL); }
static inline uint64_t bm_and(uint64_t *p, uint64_t v) { return SP_ATOMIC_FETCH_AND(p, v, __ATOMIC_ACQ_REL); }
static inline uint64_t bm_load(const uint64_t *p) { return SP_ATOMIC_LOAD(p, __ATOMIC_ACQUIRE); }
static inline void bm_store(uint64_t *p, uint64_t v) { SP_ATOMIC_STORE(p, v, __ATOMIC_RELEASE); }
#endif
/* The claim of an object slot writes the current epoch's young word, which
   has one writer: the chunk's owner, on its own thread. Nothing else sets or
   clears a bit there -- the sweep works the epoch that closed, the mark
   writes the old and mark words, the pools that used to hand a slot back
   (sp_slab_relive) are bypassed with the slab on -- so the claim is a plain
   read-modify-write on both builds. A payload's claim word is pin, which a
   freeze or a sweep on another thread writes too, and stays atomic. */
#ifdef SP_THREADS
/* atomic load and store, not a locked instruction: the sweeper reads this
   word (a parked header is one in no generation), and the model wants the
   two sides atomic; with one writer the read-modify-write loses nothing.
   Aging's carry into the current epoch would be a second writer, which is
   one more reason it stays off with the slab on. */
static inline uint64_t bm_or_owned(uint64_t *p, uint64_t v) { uint64_t o = SP_ATOMIC_LOAD(p, __ATOMIC_RELAXED); SP_ATOMIC_STORE(p, o | v, __ATOMIC_RELEASE); return o; }
#else
static inline uint64_t bm_or_owned(uint64_t *p, uint64_t v) { uint64_t o = *p; *p = o | v; return o; }
#endif
#ifndef SP_THREADS
static inline uint64_t bm_or(uint64_t *p, uint64_t v) { uint64_t o = *p; *p = o | v; return o; }
static inline uint64_t bm_and(uint64_t *p, uint64_t v) { uint64_t o = *p; *p = o & v; return o; }
static inline uint64_t bm_load(const uint64_t *p) { return *p; }
static inline void bm_store(uint64_t *p, uint64_t v) { *p = v; }
#endif

/* One reservation, aligned to the arena size so a slot's arena and chunk are
   address arithmetic. Mapped twice the size and trimmed to the aligned
   middle; the pages are untouched until a chunk is carved, so the size costs
   nothing but address space. */
static void sp_slab_reserve(void) {
#ifdef __wasi__
  /* wasm has one linear memory that only grows: nothing to reserve, trim or
     give back. 64 MB is taken as one aligned block (an engine commits its
     pages as they are touched); past it every block is a malloc. */
  { size_t want = (size_t)64 << 20; void *m = NULL;
    if (posix_memalign(&m, SP_SLAB_ARENA, want) == 0) { sp_slab_base = sp_slab_brk = (uintptr_t)m; sp_slab_cap = want; }
    else sp_slab_on = 0;
    return; }
#endif
  /* 16 GB of address space on a 64-bit host; a 32-bit one has 2 to 3 GB
     for everything, so it asks for 512 MB and settles for what it gets
     (16 << 30 is 0 in its size_t, which asked for nothing and turned the
     slab off) */
  size_t want = sizeof(void *) >= 8 ? (size_t)16 << 30 : (size_t)512 << 20;
  while (want >= ((size_t)64 << 20)) {
    size_t len = want + SP_SLAB_ARENA;
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m != MAP_FAILED) {
      uintptr_t lo = (uintptr_t)m, base = (lo + SP_SLAB_ARENA - 1) & ~(SP_SLAB_ARENA - 1);
      if (base > lo) munmap((void *)lo, base - lo);
      uintptr_t hi = lo + len, top = base + want;
      if (hi > top) munmap((void *)top, hi - top);
      sp_slab_base = sp_slab_brk = base;
      sp_slab_cap = want;
      /* Keep the untouched reservation out of a core dump: the kernel writes a
         mapping's every page, zero or not, and 16 GB of them took a crashing
         program 20 s to die (a minute and more on CI). The arenas actually
         carved are put back in, one at a time, as they are handed out. */
#ifdef MADV_DONTDUMP
      madvise((void *)base, want, MADV_DONTDUMP);
#endif
      return;
    }
    want >>= 2;
  }
  sp_slab_on = 0;   /* no range at all: every block is a malloc */
}

/* Is jemalloc the process's malloc? Its thread caches did what the free
   list did, and measured beside them the free-list slab was a loss
   (campfire's room page 2,680 req/s with jemalloc alone against 2,470 with
   the slab on top: the per-free atomics landed on caches that were already
   free-list pops), so the slab used to default off when jemalloc was
   present. The bitmap sweep changed the comparison: with it the slab under
   jemalloc answers the same requests a second as jemalloc alone (5,570
   against 5,560) on 17% less CPU and 13% less memory, the part jemalloc's
   caches cannot do being the sweep that never touches the dead. So the slab
   is on regardless now; the detection stays for the report and for
   SPINEL_GC_SLAB=0. Detection is jemalloc's own mallctl symbol, resolved by
   the dynamic linker from a linked or preloaded jemalloc; nothing else
   defines it. */
#if defined(__APPLE__)
#include <dlfcn.h>
static int sp_slab_jemalloc_present(void) { return dlsym(RTLD_DEFAULT, "mallctl") != NULL; }
#elif SP_HAVE_WEAK
extern int mallctl(const char *, void *, size_t *, void *, size_t) SP_WEAK;
static int sp_slab_jemalloc_present(void) { return mallctl != NULL; }
#else
/* no weak symbols (sp_compat.h): jemalloc goes unreported */
static int sp_slab_jemalloc_present(void) { return 0; }
#endif

#if defined(__GLIBC__)
#include <malloc.h>
#endif
static void sp_slab_init(void) {
  const char *e = getenv("SPINEL_GC_SLAB");
#if defined(__GLIBC__)
  /* The blocks past the slab's largest class are malloc's, and glibc trims
     the top of its heap whenever a free leaves 128 KB there: a program
     churning arrays of a few tens of kilobytes had the heap's top shrink
     and grow again on every one (900 brk calls and 6,000 page faults on a
     20 ms benchmark). The collector returns memory itself, with a
     malloc_trim after every full cycle, so the automatic trim can wait
     for a larger top. Setting any of these switches off glibc's dynamic
     mmap threshold (which climbs as large blocks are freed), so it is set
     here too: left at its 128 KB start, every page a server rendered
     (400 KB) was an mmap and a munmap, 140 page faults a request, and a
     test building a megabyte string 4,800 times faulted 760,000 pages.
     A megabyte of top and a 4 MB mmap threshold: a server's arenas keep
     at most a megabyte of free top each (32 MB kept 100 MB more resident
     across 33 workers, and 4 MB 60 MB more), and its 400 KB pages stay
     on the heap. */
  mallopt(M_TRIM_THRESHOLD, 1 << 20);
  mallopt(M_TOP_PAD, 1 << 20);
  mallopt(M_MMAP_THRESHOLD, 4 << 20);
#endif
  int c = 0;
  for (unsigned i = 0; i <= SP_SLAB_MAX / 16; i++) {
    while (c < SP_SLAB_NCLS - 1 && sp_slab_csize[c] < i * 16) c++;
    sp_slab_cls_of[i] = (uint8_t)c;
  }
  for (int k = 0; k < SP_SLAB_NCLS; k++) {
    unsigned cs = sp_slab_csize[k];
    sp_slab_nslots_of[k] = (uint16_t)(SP_SLAB_CHUNK / cs);
    /* the smallest reciprocal that divides every in-chunk offset exactly */
    uint32_t r = (uint32_t)((((uint64_t)1 << 32) + cs - 1) / cs);
    for (unsigned off = 0; off < SP_SLAB_CHUNK; off += 16)
      if ((unsigned)(((uint64_t)off * r) >> 32) != off / cs) { r = 0; break; }
    sp_slab_recip[k] = r;   /* 0: divide (never, checked here) */
  }
  if (e && *e) sp_slab_on = (*e != '0');
  else sp_slab_on = 1;
  (void)sp_slab_jemalloc_present;
  if (sp_slab_on) sp_slab_reserve();
}

static inline sp_slab_chunk *sp_slab_chunk_of(const void *p) {
  uintptr_t a = (uintptr_t)p;
  sp_slab_arena *ar = (sp_slab_arena *)(a & ~(SP_SLAB_ARENA - 1));
  return &ar->ch[(a & (SP_SLAB_ARENA - 1)) / SP_SLAB_CHUNK];
}
static inline char *sp_slab_chunk_base(sp_slab_chunk *ch) {
  sp_slab_arena *ar = (sp_slab_arena *)((uintptr_t)ch & ~(SP_SLAB_ARENA - 1));
  return (char *)ar + (size_t)(ch - ar->ch) * SP_SLAB_CHUNK;
}
static inline sp_slab_bm *sp_slab_bm_of(sp_slab_chunk *ch) {
  sp_slab_arena *ar = (sp_slab_arena *)((uintptr_t)ch & ~(SP_SLAB_ARENA - 1));
  return (sp_slab_bm *)((char *)ar + SP_SLAB_CHUNK) + (ch - ar->ch);
}
/* a pointer anywhere inside a slot names it: the chunk, its bitmaps, and the
   slot's word and bit */
typedef struct { sp_slab_chunk *ch; sp_slab_bm *bm; unsigned w; uint64_t bit; unsigned idx; } sp_slab_loc;
static inline void sp_slab_locate(const void *p, sp_slab_loc *l) {
  uintptr_t a = (uintptr_t)p;
  l->ch = sp_slab_chunk_of(p);
  l->bm = sp_slab_bm_of(l->ch);
  unsigned off = (unsigned)(a & (SP_SLAB_CHUNK - 1));
  unsigned cls = l->ch->cls;
  unsigned idx = sp_slab_recip[cls] ? (unsigned)(((uint64_t)off * sp_slab_recip[cls]) >> 32) : off / sp_slab_csize[cls];
  l->idx = idx;
  l->w = idx >> 6;
  l->bit = (uint64_t)1 << (idx & 63);
}

/* The next arena of the reservation: its header table and bitmaps are zero
   already (an untouched page reads as zero), so only the empty list needs
   writing. */
static int sp_slab_next_arena(void) {
  if (sp_slab_brk + SP_SLAB_ARENA > sp_slab_base + sp_slab_cap) return 0;
  sp_slab_arena *ar = (sp_slab_arena *)sp_slab_brk;
  sp_slab_brk += SP_SLAB_ARENA;
#ifdef MADV_DODUMP
  madvise((void *)ar, SP_SLAB_ARENA, MADV_DODUMP);   /* this arena holds objects: dump it */
#endif
#ifdef MADV_HUGEPAGE
  /* An arena is 4 MB, aligned: two huge pages where the kernel offers them
     (transparent_hugepage=madvise, the common setting). The program then
     faults an arena in twice instead of a thousand times, and walks it on
     two TLB entries; a list benchmark spent a third of its time in those
     faults. */
  { static int huge = -1;
    if (huge < 0) { const char *e = getenv("SPINEL_SLAB_HUGE"); huge = e ? atoi(e) : 1; }   /* SPINEL_SLAB_HUGE=0 turns it off */
    if (huge) madvise((void *)ar, SP_SLAB_ARENA, MADV_HUGEPAGE); }
#endif
  for (int i = (int)SP_SLAB_NCHUNK - 1; i >= SP_SLAB_FIRST; i--) {   /* pops then ascend in address */
    ar->ch[i].next_avail = sp_slab_empty;
    sp_slab_empty = &ar->ch[i];
  }
  return 1;
}

/* A free slot of the chunk, claimed into the current epoch's young word
   (or, for a payload, into old and pin). The owner is the only claimer, so
   the search reads the three generation words plainly: a bit a sweeper
   clears meanwhile is a slot it may find next time, and no bit it reads as
   clear can be set by anyone but itself. The claim is an atomic or all the
   same, because a sweep carrying an aged survivor into the current epoch
   writes the same word. NULL when the chunk has no free slot. */
/* The lowest run of consecutive free bits of the cached word, claimed
   whole for one kind and installed as its bump range. 0 when the word
   has none left. */
static int sp_slab_run(sp_slab_worker *wk, int cls, int payload) {
  uint64_t m = wk->wmask[cls];
  unsigned csize = sp_slab_csize[cls];
  uint64_t *claim = payload ? wk->wpin[cls] : wk->wyoung[cls];
  while (m) {
    uint64_t low = m & -m;
    uint64_t run = m & ~(m + low);   /* the carry from the lowest bit stops at the run's end */
    wk->wmask[cls] = m & ~run;
    uint64_t was = payload ? bm_or(claim, run) : bm_or_owned(claim, run);
    if (SP_EXPECT(was & run, 0)) {
      /* a bit of it was taken meanwhile (a pin on a slot the sweep is
         finalizing: only the pin word has other writers); keep what came
         before the first such bit, unclaim the rest */
      uint64_t bad = was & run;
      uint64_t keep = run & ((bad & -bad) - 1);
      bm_and(claim, ~(run & ~keep));
      run = keep;
      if (!run) { m = wk->wmask[cls]; continue; }
    }
    unsigned i = (unsigned)SP_CTZ64(run), n = (unsigned)SP_POPCOUNT64(run);
    wk->rnext[payload][cls] = wk->wbase[cls] + (size_t)i * csize;
    wk->rend[payload][cls] = wk->rnext[payload][cls] + (size_t)n * csize;
    wk->rbase[payload][cls] = wk->wbase[cls];
    wk->rclaim[payload][cls] = claim;
    wk->rfin[payload][cls] = wk->wfin[cls];
    wk->rstr[payload][cls] = wk->wstr[cls];
    return 1;
  }
  return 0;
}
static inline void *sp_slab_take(sp_slab_worker *wk, int cls, sp_slab_chunk *ch, unsigned csize, int payload) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6;
  unsigned w = ch->hint;
  unsigned e = sp_slab_epoch & 1;
  wk->wmask[cls] = 0;
  for (unsigned n = 0; n < nw; n++, w = (w + 1 == nw) ? 0 : w + 1) {
    uint64_t tail = (w == nw - 1 && (ch->nslots & 63)) ? ~(uint64_t)0 << (ch->nslots & 63) : 0;
    /* The four words are not one snapshot. The one writer that moves a
       slot from used to used on another thread is a pool handing a parked
       header back (sp_slab_relive): it sets the young bit and THEN clears
       the pin, so reading pin first and young after cannot see the slot
       in neither -- the pin is either still set, or young already is. A
       free (sp_slab_free) only ever turns used into free. The claim's own
       result is still checked (sp_slab_run). */
    uint64_t used = bm_load(&bm->old[w]) | bm_load(&bm->pin[w]);
    used |= bm_load(&bm->young[0][w]) | bm_load(&bm->young[1][w]) | tail;
    if (used == ~(uint64_t)0) continue;
    ch->hint = (uint16_t)w;
    /* the word, for the fast path: its claim word (this epoch's parity,
       or pin for a payload), its free bits, its first slot */
    wk->wmask[cls] = ~used;
    wk->wyoung[cls] = &bm->young[e][w];
    wk->wpin[cls] = &bm->pin[w];
    wk->wfin[cls] = &bm->fin[w];
    wk->wstr[cls] = &bm->str[w];
    wk->wbase[cls] = sp_slab_chunk_base(ch) + (size_t)(w << 6) * csize;
    if (sp_slab_run(wk, cls, payload)) {
      char *p = wk->rnext[payload][cls];
      wk->rnext[payload][cls] = p + csize;
      if (sp_slab_verify_on) sp_slab_note(p, 40 + (int)e + (payload ? 2 : 0));
      return p;
    }
  }
  return NULL;
}

/* The slow path: the worker's current chunk for this class has no slot. NULL
   when the reservation is exhausted, and the caller mallocs. */
static SP_NOINLINE void *sp_slab_refill(sp_slab_worker *wk, int cls, unsigned csize, int payload) {
  sp_slab_chunk *ch;
  void *p = NULL;
  for (;;) {
#ifdef SP_THREADS
    /* pushed by a sweeper beside the running program (a compare-and-swap),
       popped only by the owner: the same exchange, with no ABA to lose to */
    ch = SP_ATOMIC_LOAD(&wk->avail[cls], __ATOMIC_ACQUIRE);
    while (ch) {
      sp_slab_chunk *nx = ch->next_avail;
      if (SP_ATOMIC_CAS(&wk->avail[cls], &ch, nx, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
#else
    ch = wk->avail[cls];
    if (ch) wk->avail[cls] = ch->next_avail;
#endif
    if (!ch) break;
    ch->next_avail = NULL;
    SP_ATOMIC_STORE(&ch->on_avail, 0, __ATOMIC_RELEASE);
    /* A sweeper decides "not the current chunk" from a read that races with
       this function installing exactly that chunk, so the chunk we are
       allocating from can land on the list too. It has already been used up
       from the front by then: take a slot if one is left, else drop it and
       move on. */
    ch->hint = 0;
    p = sp_slab_take(wk, cls, ch, csize, payload);
    if (p) break;
  }
  if (!ch) {
    SP_SLAB_LOCK();
    if (!sp_slab_empty && !sp_slab_next_arena()) { SP_SLAB_UNLOCK(); return NULL; }
    ch = sp_slab_empty;
    sp_slab_empty = ch->next_avail;
    SP_SLAB_UNLOCK();
    ch->next_avail = NULL;
    ch->nslots = sp_slab_nslots_of[cls];
    ch->hint = 0;
    ch->cls = (uint16_t)cls;
    ch->wid = (uint16_t)(wk - sp_slab_wk);
    ch->on_avail = 0;
    ch->in_use = 1;
    /* onto the owner's owned list: the sweep of this worker walks it */
    ch->own_prev = NULL;
    ch->own_next = wk->owned;
    if (wk->owned) wk->owned->own_prev = ch;
    SP_ATOMIC_STORE(&wk->owned, ch, __ATOMIC_RELEASE);
    p = sp_slab_take(wk, cls, ch, csize, payload);
  }
  SP_ATOMIC_STORE(&wk->cur[cls], ch, __ATOMIC_RELEASE);
  wk->taken++; wk->taken_cls[cls]++;
  ch->touched = 1;
  return p;
}

/* Zero a block of a class's size: 16 bytes a store, the count from the
   size, entered at the store that leaves exactly the block. Sizes above
   256 go to memset, which earns its call there. */
typedef struct { uint64_t a, b; } sp_slab_u16;
static SP_INLINE void sp_slab_zero(void *p, unsigned csize) {
  sp_slab_u16 *q = (sp_slab_u16 *)p;
  const sp_slab_u16 z = { 0, 0 };
  switch (csize >> 4) {
  case 16: q[15] = z; /* fallthrough */
  case 15: q[14] = z; /* fallthrough */
  case 14: q[13] = z; /* fallthrough */
  case 13: q[12] = z; /* fallthrough */
  case 12: q[11] = z; /* fallthrough */
  case 11: q[10] = z; /* fallthrough */
  case 10: q[9] = z; /* fallthrough */
  case 9: q[8] = z; /* fallthrough */
  case 8: q[7] = z; /* fallthrough */
  case 7: q[6] = z; /* fallthrough */
  case 6: q[5] = z; /* fallthrough */
  case 5: q[4] = z; /* fallthrough */
  case 4: q[3] = z; /* fallthrough */
  case 3: q[2] = z; /* fallthrough */
  case 2: q[1] = z; /* fallthrough */
  case 1: q[0] = z; return;
  default: memset(p, 0, csize); return;
  }
}
/* the same for a block known to be 256 bytes or less: no call on any path,
   which is what lets sp_gc_alloc's front run without a frame */
static SP_INLINE void sp_slab_zero_small(void *p, unsigned csize) {
  sp_slab_u16 *q = (sp_slab_u16 *)p;
  const sp_slab_u16 z = { 0, 0 };
  switch (csize >> 4) {
  case 16: q[15] = z; /* fallthrough */
  case 15: q[14] = z; /* fallthrough */
  case 14: q[13] = z; /* fallthrough */
  case 13: q[12] = z; /* fallthrough */
  case 12: q[11] = z; /* fallthrough */
  case 11: q[10] = z; /* fallthrough */
  case 10: q[9] = z; /* fallthrough */
  case 9: q[8] = z; /* fallthrough */
  case 8: q[7] = z; /* fallthrough */
  case 7: q[6] = z; /* fallthrough */
  case 6: q[5] = z; /* fallthrough */
  case 5: q[4] = z; /* fallthrough */
  case 4: q[3] = z; /* fallthrough */
  case 3: q[2] = z; /* fallthrough */
  case 2: q[1] = z; /* fallthrough */
  case 1: q[0] = z; return;
  default: SP_UNREACHABLE();
  }
}
/* The slow half of an allocation: the cached word is empty, so search the
   current chunk, else refill; past the largest class, or with the slab
   off, malloc. */
static SP_NOINLINE void *sp_slab_alloc_slow(size_t need, int payload, int zero) {
  if (SP_EXPECT(sp_slab_on < 0, 0)) sp_slab_init();
  if (sp_slab_on && need <= SP_SLAB_MAX) {
    int cls = sp_slab_cls_of[(need + 15) >> 4];
    unsigned csize = sp_slab_csize[cls];
    sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
    sp_slab_chunk *ch = wk->cur[cls];
    void *p = NULL;
    /* the next run of the cached word, if the word is of this kind */
    if (sp_slab_run(wk, cls, payload)) {
      p = wk->rnext[payload][cls]; wk->rnext[payload][cls] = (char *)p + csize;
      if (sp_slab_verify_on) sp_slab_note(p, 40 + (int)(sp_slab_epoch & 1) + (payload ? 2 : 0));
    }
    if (!p && ch) p = sp_slab_take(wk, cls, ch, csize, payload);
    if (!p) p = sp_slab_refill(wk, cls, csize, payload);
    if (p) {
      if (zero) memset(p, 0, need);
      return p;
    }
  }
  void *p = zero ? calloc(1, need) : malloc(need);
  if (!p) sp_oom_die();
  return p;
}
/* The fast half, inlined into the entry points: the next slot of the
   claimed run, a bump. The claim was made for the whole run when it was
   cut (sp_slab_run), so the bitmap is not touched here; an object with a
   finalizer sets its fin bit, in the word cached beside the claim word. */
static SP_INLINE void *sp_slab_alloc_in(size_t need, int payload, int fin, int zero) {
  if (SP_EXPECT(sp_slab_on > 0 && need <= SP_SLAB_MAX, 1)) {
    int cls = sp_slab_cls_of[(need + 15) >> 4];
    sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
    char *p = wk->rnext[payload][cls];
    if (SP_EXPECT(p != wk->rend[payload][cls], 1)) {
      unsigned csize = sp_slab_csize[cls];
      wk->rnext[payload][cls] = p + csize;
      if (fin) {
        unsigned i = (unsigned)(((uint64_t)(size_t)(p - wk->rbase[payload][cls]) * sp_slab_recip[cls]) >> 32);
        bm_or(wk->rfin[payload][cls], (uint64_t)1 << i);
      }
      /* zeroed slot by slot, the class size in 16-byte stores entered
         mid-run (a memset call, or a counted loop, costs more than the
         bytes at these sizes). Zeroing a whole free word in one memset at
         the refill measured worse: the string stores miss the cache on
         purpose, and the demand fills quintupled. */
      if (zero) sp_slab_zero(p, csize);
      if (SP_EXPECT(sp_slab_verify_on, 0)) sp_slab_note(p, 40 + (int)(sp_slab_epoch & 1) + (payload ? 2 : 0));
      return p;
    }
  }
  void *p = sp_slab_alloc_slow(need, payload, zero);
  if (fin && sp_slab_on > 0 && sp_slab_owns(p)) {
    sp_slab_loc l; sp_slab_locate(p, &l);
    bm_or(&l.bm->fin[l.w], l.bit);
  }
  return p;
}
/* An object with its header written, in one call: the fast path above, the
   zeroing, and the three header fields the collector reads (the flags are
   zero already). What sp_gc_alloc used to do in two calls and a memset. */
void *sp_slab_alloc_obj(size_t need, void (*fin)(void *), void (*scn)(void *)) {
  void *p = sp_slab_alloc_in(need, 0, fin != NULL, 1);
  sp_gc_hdr *h = (sp_gc_hdr *)p;
  h->finalize = fin; h->scan = scn; h->size = need;
  if (SP_EXPECT(sp_slab_verify_on, 0)) sp_slab_note(p, 1);
  return p;
}
/* The object allocation of the program, moved here from lib/sp_alloc.c so
   that the claim above, the header, and the collector's bookkeeping (the
   trigger, the byte count, the report) are one frame: what was two calls
   deep is inlined into one. The semantics are sp_alloc.c's. */
/* The lean front of sp_gc_alloc: no call on its path, so the compiler
   keeps it in caller-saved registers with no frame to build (every other
   case tail-calls the full form below). It runs when the slab is on and
   nothing wants a look at each allocation (the verifier, the report, the
   stress switch not yet read): sp_gc_alloc_fast_ok, recomputed by the full
   form and dropped by whoever turns one of those on. */
int sp_gc_alloc_fast_ok = 0;
static void *sp_gc_alloc_full(size_t sz, void (*fin)(void *), void (*scn)(void *));
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  size_t need = sizeof(sp_gc_hdr) + sz;
  if (SP_EXPECT(!sp_gc_alloc_fast_ok || need > 256, 0)) return sp_gc_alloc_full(sz, fin, scn);
  if (SP_EXPECT(SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold), 0)) return sp_gc_alloc_full(sz, fin, scn);
  int cls = sp_slab_cls_of[(need + 15) >> 4];
  sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
  char *p = wk->rnext[0][cls];
  if (SP_EXPECT(p == wk->rend[0][cls], 0)) return sp_gc_alloc_full(sz, fin, scn);
  unsigned csize = sp_slab_csize[cls];
  wk->rnext[0][cls] = p + csize;
  if (fin) {
    unsigned i = (unsigned)(((uint64_t)(size_t)(p - wk->rbase[0][cls]) * sp_slab_recip[cls]) >> 32);
    bm_or(wk->rfin[0][cls], (uint64_t)1 << i);
  }
  sp_slab_zero_small(p, csize);
  sp_gc_hdr *h = (sp_gc_hdr *)p;
  h->finalize = fin; h->scan = scn; h->size = need;
  sp_gc_bytes_add(need);
  return p + sizeof(sp_gc_hdr);
}
static SP_NOINLINE void *sp_gc_alloc_full(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
#ifdef SP_THREADS
  /* Lock-free fast path: the list push is a CAS (SP_GC_HEAP_PUSH) and the live-
     byte counter is atomic, so concurrent allocations need no mutex -- the old
     sp_heap_lock only serialized them and the string sweep, and both string
     allocation (per-worker heap) and every collection (stop-the-world) have
     moved off it. Removals happen only under stop-the-world with every mutator
     parked, so a push never races the sweep. The stress-threshold one-shot is
     idempotent under a race. */
  if (!sp_gc_stress_checked) { sp_gc_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { SP_GC_CTR_SET(sp_gc_threshold, 2048); sp_gc_threshold_init = 2048; sp_gc_stress_pin = 1; } }
  if (SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold)) sp_stw_collect();
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)sp_slab_alloc_in(need, 0, fin != NULL, 1);
  h->finalize = fin; h->scan = scn; h->size = need;
  if (SP_EXPECT(sp_alloc_report_on, 0)) sp_alloc_report_count((void *)scn, sz);
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
  if (SP_EXPECT(sp_slab_verify_on, 0)) sp_slab_note(h, 1);
  sp_gc_alloc_fast_ok = sp_gc_stress_checked && sp_slab_on > 0 && !sp_slab_verify_on && !sp_alloc_report_on;
  return (char *)h + sizeof(sp_gc_hdr);
#else
  SP_HEAP_LOCK();
  /* The threshold store is atomic: sp_gc_collection_wanted reads it without
     the heap lock. threshold_init stays plain -- only retune reads it, under
     stop-the-world, ordered after this by the writer's park. */
  if (!sp_gc_stress_checked) { sp_gc_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { SP_GC_CTR_SET(sp_gc_threshold, 2048); sp_gc_threshold_init = 2048; sp_gc_stress_pin = 1; } }
  if (SP_GC_CTR_GET(sp_gc_bytes) > sp_gc_threshold) {
    sp_gc_collect_retune();
  }
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)sp_slab_alloc_in(need, 0, fin != NULL, 1);
  h->finalize = fin; h->scan = scn; h->size = need;
  if (SP_EXPECT(sp_alloc_report_on, 0)) sp_alloc_report_count((void *)scn, sz);
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
  if (SP_EXPECT(sp_slab_verify_on, 0)) sp_slab_note(h, 1);
  sp_gc_alloc_fast_ok = sp_gc_stress_checked && sp_slab_on > 0 && !sp_slab_verify_on && !sp_alloc_report_on;
  SP_HEAP_UNLOCK();
  return (char *)h + sizeof(sp_gc_hdr);
#endif
}
/* A block the collector will sweep: an object (zeroed) or a heap string */
void *sp_slab_alloc(size_t need) {
  void *p = sp_slab_alloc_in(need, 0, 0, 1);
  if (SP_EXPECT(sp_slab_verify_on, 0)) sp_slab_note(p, 1);
  return p;
}
/* A heap string: the bump of sp_slab_alloc_in with the str bit set in the
   word cached beside the run, the way an object's fin bit is. Locating the
   slot from its address instead (chunk, bitmap, word, bit) was two thirds of
   the cost of allocating a short string. */
void *sp_slab_alloc_str(size_t need) {
  if (SP_EXPECT(sp_slab_on > 0 && need <= SP_SLAB_MAX && !sp_slab_verify_on, 1)) {
    int cls = sp_slab_cls_of[(need + 15) >> 4];
    sp_slab_worker *wk = &sp_slab_wk[SP_SLAB_WID()];
    char *q = wk->rnext[0][cls];
    if (SP_EXPECT(q != wk->rend[0][cls], 1)) {
      wk->rnext[0][cls] = q + sp_slab_csize[cls];
      unsigned i = (unsigned)(((uint64_t)(size_t)(q - wk->rbase[0][cls]) * sp_slab_recip[cls]) >> 32);
      bm_or(wk->rstr[0][cls], (uint64_t)1 << i);
      return q;
    }
  }
  void *p = sp_slab_alloc_in(need, 0, 0, 0);
  if (sp_slab_on > 0 && sp_slab_owns(p)) {
    sp_slab_loc l; sp_slab_locate(p, &l);
    bm_or(&l.bm->str[l.w], l.bit);
    if (sp_slab_verify_on) sp_slab_note(p, 2);
  }
  return p;
}
/* A block no sweep frees: a container's payload, dying by an explicit free */
void *sp_slab_alloc_raw(size_t need) {
  void *p = sp_slab_alloc_in(need, 1, 0, 0);
  if (sp_slab_verify_on) sp_slab_note(p, 3);
  return p;
}
/* the header gained a finalizer or a recycler after its allocation */
void sp_slab_set_fin(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  bm_or(&l.bm->fin[l.w], l.bit);
  if (sp_slab_verify_on) sp_slab_note(h, 4);
}
/* a frozen heap string: kept until the program ends, as a static literal is */
void sp_slab_pin(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) return;
  sp_slab_loc l; sp_slab_locate(p, &l);
  bm_or(&l.bm->pin[l.w], l.bit);
  if (sp_slab_verify_on) sp_slab_note(p, 9);
}
/* A dead header about to be handed to its pool: pinned, out of every
   sweep's reach, until the pool hands it out again or frees it. Its
   finalizer bit stays: a pinned slot is never dead, and the bit is what
   makes its next death run the recycler again. */
void sp_slab_park(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  bm_or(&l.bm->pin[l.w], l.bit);
}
/* A pooled header handed out again: into the current epoch, and unpinned
   -- in that order, since the owner's search reads pin before young
   (sp_slab_take) and must never find the slot in neither. */
void sp_slab_relive(void *h) {
  if (sp_slab_on <= 0 || !sp_slab_owns(h)) return;
  sp_slab_loc l; sp_slab_locate(h, &l);
  bm_or(&l.bm->young[sp_slab_epoch & 1][l.w], l.bit);
  bm_and(&l.bm->pin[l.w], ~l.bit);
  if (sp_slab_verify_on) sp_slab_note(h, 5 + 10 * (int)(sp_slab_epoch & 1));
}
/* ---- SPINEL_GC_STRESS=2: the quarantine ----
   A slot a sweep frees is the next slot its class hands out, and until then
   it keeps the dead object's bytes: the sweep reads the bitmaps and never
   the slot. So a reference the collector was never told about reads the
   object it lost, intact, or a live object of the same size in its place,
   and the program answers right by luck. Here a freed slot is filled with
   0xdb and kept out of reuse: pinned, so no sweep and no allocation touches
   it, and named in a bitmap of its own, which is what tells it from a
   payload. An object keeps its header, and a string its header, marker byte
   and length, so a report can say what the slot was; the bytes after are
   poison. sp_slab_is_live answers no for such a slot, so the verifier's
   membership test (sp_gc_mark) stops a mark that reaches one and names the
   root or the holder, and a read by the program gets 0xdb: an Integer field
   is -2604246222170760229, a pointer field faults, a string is its own
   length of \xDB.
   The quarantine holds SP_SLAB_QUAR_MAX bytes and is then let go whole, at
   the next barrier: every chunk with a slot in it is one more for each
   sweep to walk, and with a collection at every allocation that walk is the
   run's cost. A slot let go keeps its poison until a claim zeroes it.
   Blocks past the largest class are malloc's, as before. */
int sp_slab_quar_on = 0;                 /* asked for, before main (lib/sp_gc.c) */
static uint64_t *sp_slab_quar = NULL;    /* SP_SLAB_NW words a chunk, over the reservation; mapped at the first barrier */
static size_t sp_slab_quar_bytes = 0;
#ifndef SP_SLAB_QUAR_MAX
#define SP_SLAB_QUAR_MAX ((size_t)1 << 20)
#endif
#define SP_SLAB_POISON 0xdb
static inline uint64_t *sp_slab_quar_of(sp_slab_chunk *ch) {
  return sp_slab_quar + ((uintptr_t)sp_slab_chunk_base(ch) - sp_slab_base) / SP_SLAB_CHUNK * SP_SLAB_NW;
}
/* the slots of one bitmap word a sweep found dead (`strs`: which of them are
   strings), or the one slot of an explicit free (`whole`: no header to keep) */
static SP_NOINLINE void sp_slab_quarantine(sp_slab_chunk *ch, sp_slab_bm *bm, unsigned w, uint64_t dead, uint64_t strs, int whole) {
  unsigned csize = sp_slab_csize[ch->cls];
  bm_or(&bm->pin[w], dead);
  bm_or(&sp_slab_quar_of(ch)[w], dead);
  for (uint64_t v = dead; v; v &= v - 1) {
    unsigned b = (unsigned)SP_CTZ64(v);
    char *slot = sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize;
    if (whole) memset(slot, SP_SLAB_POISON, csize);
    else if ((strs >> b) & 1) {
      size_t room = csize - sizeof(sp_str_hdr) - 2, len = ((sp_str_hdr *)slot)->len;
      memset(slot + sizeof(sp_str_hdr) + 1, SP_SLAB_POISON, len < room ? len : room);
    }
    else memset(slot + sizeof(sp_gc_hdr), SP_SLAB_POISON, csize - sizeof(sp_gc_hdr));
  }
#ifdef SP_THREADS
  SP_ATOMIC_FETCH_ADD(&sp_slab_quar_bytes, (size_t)SP_POPCOUNT64(dead) * csize, __ATOMIC_RELAXED);
#else
  sp_slab_quar_bytes += (size_t)SP_POPCOUNT64(dead) * csize;
#endif
}
int sp_slab_is_quarantined(const void *p) {
  if (!sp_slab_quar || !sp_slab_owns(p)) return 0;
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (bm_load(&sp_slab_quar_of(l.ch)[l.w]) & l.bit) != 0;
}

int sp_slab_is_str(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (l.bm->str[l.w] & l.bit) != 0;
}
/* the verifiers' report: what the bitmaps say about a slot */
void sp_slab_describe(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) { fprintf(stderr, "  slab: not a slab block\n"); return; }
  sp_slab_loc l; sp_slab_locate(p, &l);
  fprintf(stderr, "  slab: chunk wid=%u cls=%u (%u B) in_use=%d slot=%u epoch=%u young0=%d young1=%d old=%d mark=%d fin=%d str=%d pin=%d\n",
          l.ch->wid, l.ch->cls, sp_slab_csize[l.ch->cls], l.ch->in_use, l.idx, sp_slab_epoch,
          !!(l.bm->young[0][l.w] & l.bit), !!(l.bm->young[1][l.w] & l.bit), !!(l.bm->old[l.w] & l.bit),
          !!(l.bm->mark[l.w] & l.bit), !!(l.bm->fin[l.w] & l.bit), !!(l.bm->str[l.w] & l.bit), !!(l.bm->pin[l.w] & l.bit));
}
/* SPINEL_GC_VERIFY: every chunk's bitmaps against their invariants -- a
   finalizer, string or pin bit on a slot in no generation is a slot that was
   freed without being cleared, or claimed without being free. */
int sp_slab_verify_on = 0;   /* declared above */
/* under the verifier: who last set a slot's bits (a byte per 32-byte slot
   over the whole reservation, touched only where slots are) */
typedef struct { unsigned char what, wid; unsigned short cycle; } sp_slab_shadow_ev;
typedef struct { unsigned char n; sp_slab_shadow_ev ev[8]; } sp_slab_shadow_rec;
static sp_slab_shadow_rec *sp_slab_shadow = NULL;
extern int sp_gc_cycle;
#define SP_SHADOW(p) (sp_slab_shadow ? &sp_slab_shadow[((uintptr_t)(p) - sp_slab_base) >> 5] : NULL)
void sp_slab_note(const void *p, int what) {
  if (!sp_slab_verify_on || !sp_slab_owns(p)) return;
  if (!sp_slab_shadow) {
    void *m = mmap(NULL, (sp_slab_cap >> 5) * sizeof(sp_slab_shadow_rec), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return;
    sp_slab_shadow = (sp_slab_shadow_rec *)m;
  }
  sp_slab_shadow_rec *sh = SP_SHADOW(p);
  if (!sh) return;
  sp_slab_shadow_ev *e = &sh->ev[sh->n++ & 7];
  e->what = (unsigned char)what; e->wid = (unsigned char)sp_worker_id_shadow(); e->cycle = (unsigned short)sp_gc_cycle;
}
static void sp_slab_verify_chunk(sp_slab_chunk *ch, const char *when) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
  for (unsigned w = 0; w < nw; w++) {
    uint64_t gen = bm->young[0][w] | bm->young[1][w] | bm->old[w] | bm->pin[w];
    uint64_t stray = (bm->fin[w] | bm->str[w] | bm->mark[w]) & ~gen;
    uint64_t both = bm->young[0][w] & bm->young[1][w];
    uint64_t finstr = bm->fin[w] & bm->str[w];
    uint64_t ymark = (bm->young[0][w] | bm->young[1][w]) & bm->mark[w] & (when[0] == 'a' && when[1] == 't' ? ~(uint64_t)0 : 0);   /* at the barrier: no young slot carries a mark */
    uint64_t bad = stray | both | finstr | ymark;
    while (bad) {
      unsigned b = (unsigned)SP_CTZ64(bad); bad &= bad - 1;
      void *slot = sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize;
      fprintf(stderr, "*** SPINEL_GC_VERIFY: slab invariant broken at %p (%s), %s:\n", slot,
              (stray >> b) & 1 ? "bits on a free slot" : (both >> b) & 1 ? "in both epochs" : (finstr >> b) & 1 ? "finalizer bit on a string" : "a mark on a young slot", when);
      sp_slab_describe(slot);
      { const uint64_t *q = (const uint64_t *)slot;
        fprintf(stderr, "  slot words: %016llx %016llx %016llx %016llx %016llx %016llx\n",
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        sp_slab_shadow_rec *sh = SP_SHADOW(slot);
        fprintf(stderr, "  history (1 obj alloc, 2 str alloc, 3 raw alloc, 4 set_fin, 5/15 relive into y0/y1, 6 free, 7 swept dead, 8/18 parked sweeping y0/y1, 9 pin, 30/31 survived sweep of y0/y1, 40-43 take into y0/y1/(+2 payload)), oldest first; now cycle %d:\n", sp_gc_cycle);
        if (sh) for (unsigned k = 0; k < 8; k++) { sp_slab_shadow_ev *e = &sh->ev[(sh->n + k) & 7]; if (e->what) fprintf(stderr, "    what=%d worker=%d cycle=%d\n", e->what, e->wid, e->cycle); } }
      abort();
    }
  }
}
/* the verifiers' report of a slot's recorded history (SPINEL_GC_VERIFY) */
void sp_slab_history(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) return;
  sp_slab_shadow_rec *sh = SP_SHADOW(p);
  fprintf(stderr, "  history (1 obj alloc, 2 str alloc, 3 raw alloc, 4 set_fin, 5/15 relive, 6 free, 7 swept dead, 8/18 parked, 9 pin, 30/31 survived sweep, 40-43 take), oldest first; now cycle %d:\n", sp_gc_cycle);
  if (sh) for (unsigned k = 0; k < 8; k++) { sp_slab_shadow_ev *e = &sh->ev[(sh->n + k) & 7]; if (e->what) fprintf(stderr, "    what=%d worker=%d cycle=%d\n", e->what, e->wid, e->cycle); }
}
void sp_slab_verify_all(void) {
  if (sp_slab_on <= 0) return;
  sp_slab_verify_on = 1; sp_gc_alloc_fast_ok = 0;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_verify_chunk(ch, "at the barrier");
    }
  }
}
/* allocated at all: in some generation (a verifier's membership test) */
int sp_slab_is_live(const void *p) {
  if (sp_slab_on <= 0 || !sp_slab_owns(p)) return 0;
  sp_slab_loc l; sp_slab_locate(p, &l);
  if (SP_EXPECT(sp_slab_quar != NULL, 0) && (sp_slab_quar_of(l.ch)[l.w] & l.bit)) return 0;   /* pinned, and freed */
  return ((l.bm->young[0][l.w] | l.bm->young[1][l.w] | l.bm->old[l.w] | l.bm->pin[l.w]) & l.bit) != 0;
}
int sp_slab_is_old(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (l.bm->old[l.w] & l.bit) != 0;
}

/* A chunk a free put a slot back into rejoins its owner's available list,
   unless it is there already or is the chunk the owner allocates from. The
   owner's current chunk is read racily: when it is mid-refill the chunk it
   is installing can be pushed here too, and refill drops it again when it
   finds it empty. */
static void sp_slab_avail_push(sp_slab_chunk *ch) {
#ifdef SP_THREADS
  if (!SP_ATOMIC_LOAD(&ch->on_avail, __ATOMIC_RELAXED) &&
      SP_ATOMIC_LOAD(&sp_slab_wk[ch->wid].cur[ch->cls], __ATOMIC_RELAXED) != ch) {
    unsigned char was = SP_ATOMIC_EXCHANGE(&ch->on_avail, 1, __ATOMIC_ACQ_REL);
    if (!was) {
      sp_slab_chunk **head = &sp_slab_wk[ch->wid].avail[ch->cls];
      sp_slab_chunk *oh;
      do { oh = SP_ATOMIC_LOAD(head, __ATOMIC_ACQUIRE); ch->next_avail = oh;
      } while (!SP_ATOMIC_CAS(head, &oh, ch, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
  }
#else
  if (!ch->on_avail && sp_slab_wk[0].cur[ch->cls] != ch) {
    ch->on_avail = 1;
    ch->next_avail = sp_slab_wk[0].avail[ch->cls];
    sp_slab_wk[0].avail[ch->cls] = ch;
  }
#endif
}

/* An explicit free, from anywhere: a payload its container dropped, a pooled
   header over the pool's cap, a malloc'd block. The slot leaves every
   generation and loses its marks; the chunk goes back on the owner's list. */
SP_TLS unsigned long sp_slab_frees = 0;   /* this thread's explicit frees, counted (sp_gc_die_cb reads it around a recycler) */
void sp_slab_free(void *p) {
  if (!sp_slab_owns(p)) { free(p); return; }
  sp_slab_loc l; sp_slab_locate(p, &l);
  uint64_t nb = ~l.bit;
  /* only the words that hold the bit: a payload is pinned and nothing else,
     and (threaded) each clear is a locked instruction */
#define SP_SLAB_CLEAR(word) do { if (bm_load(&(word)) & l.bit) bm_and(&(word), nb); } while (0)
  SP_SLAB_CLEAR(l.bm->young[0][l.w]);
  SP_SLAB_CLEAR(l.bm->young[1][l.w]);
  SP_SLAB_CLEAR(l.bm->fin[l.w]);
  SP_SLAB_CLEAR(l.bm->str[l.w]);
  SP_SLAB_CLEAR(l.bm->mark[l.w]);
  SP_SLAB_CLEAR(l.bm->old[l.w]);
  if (SP_EXPECT(sp_slab_quar != NULL, 0)) { sp_slab_frees++; if (sp_slab_verify_on) sp_slab_note(p, 6); sp_slab_quarantine(l.ch, l.bm, l.w, l.bit, 0, 1); return; }
  SP_SLAB_CLEAR(l.bm->pin[l.w]);
#undef SP_SLAB_CLEAR
  sp_slab_frees++;
  if (sp_slab_verify_on) sp_slab_note(p, 6);
  sp_slab_avail_push(l.ch);
}

/* A container payload resized: a slab block's capacity is its size class,
   so a smaller or equal request keeps the block, a larger one moves to a
   fresh block (slab or malloc, by size) and frees the old; a malloc block
   is realloc'd, which for the largest is an mremap with no copy. */
void *sp_pl_realloc(void *p, size_t newn) {
  if (!p) return sp_slab_alloc_raw(newn);
  if (!sp_slab_owns(p)) {
    void *q = realloc(p, newn);
    if (!q) sp_oom_die();
    return q;
  }
  sp_slab_chunk *ch = sp_slab_chunk_of(p);
  size_t have = sp_slab_csize[ch->cls];
  if (newn <= have) return p;
  void *q = sp_slab_alloc_raw(newn);
  memcpy(q, p, have);
  sp_slab_free(p);
  return q;
}

/* ---- the mark ----
   The mark bit of a slot, set by whichever marker reaches it first, and
   promotion: a young slot the mark reaches becomes old here, which is what
   the concurrent sweep needs (it will not write the bit, and it frees what
   is young and unmarked). `aging` keeps a first-time survivor young: the
   sweep carries it into the next epoch instead. Returns 0 when the slot
   was already marked this cycle, else 1, with *was_young telling a promotion
   (or an aged survivor) from an old object reached again. */
int sp_slab_mark(const void *p, int aging, int *was_young) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  uint64_t o = bm_or(&l.bm->mark[l.w], l.bit);
  if (o & l.bit) { *was_young = 0; return 0; }
  uint64_t ow = bm_load(&l.bm->old[l.w]);
  if (ow & l.bit) { *was_young = 0; return 1; }
  *was_young = 1;
  if (!aging) bm_or(&l.bm->old[l.w], l.bit);
  return 1;
}
/* is the slot marked this cycle? (the string side's "already marked" test) */
int sp_slab_is_marked(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  return (bm_load(&l.bm->mark[l.w]) & l.bit) != 0;
}
/* the verifiers' probe unmarks a slot to see who marks it again */
void sp_slab_unmark(const void *p) {
  sp_slab_loc l; sp_slab_locate(p, &l);
  bm_and(&l.bm->mark[l.w], ~l.bit);
}

/* ---- the sweep ----
   Under the barrier, before the sweep of the epoch that just closed starts:
   new allocations go to the other parity from here. */
/* Under the barrier: every worker's claimed-and-not-handed-out run is
   unclaimed, and its cache dropped. Before the collector walks the young
   bits (they would name slots holding garbage) and before the epoch flips
   (the claim words name the parity that is about to close). */
void sp_slab_runs_release(void) {
  if (sp_slab_on <= 0) return;
  for (int w = 0; w < SP_SLAB_NWK; w++) {
    sp_slab_worker *wk = &sp_slab_wk[w];
    for (int c = 0; c < SP_SLAB_NCLS; c++) {
      for (int k = 0; k < 2; k++) {
        char *p = wk->rnext[k][c], *end = wk->rend[k][c];
        if (p && p < end) {
          unsigned i = (unsigned)(((uint64_t)(size_t)(p - wk->rbase[k][c]) * sp_slab_recip[c]) >> 32);
          unsigned n = (unsigned)(((uint64_t)(size_t)(end - p) * sp_slab_recip[c]) >> 32);
          uint64_t mask = (n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1)) << i;
          bm_and(wk->rclaim[k][c], ~mask);
        }
        wk->rnext[k][c] = wk->rend[k][c] = NULL;
      }
      wk->wmask[c] = 0;
    }
  }
}
/* Under the barrier, at the start of a collection, before any sweep or free
   can race it: the map is made at the first one, and a quarantine past its
   size is let go whole. */
void sp_slab_quarantine_trim(void) {
  if (!sp_slab_quar) {
    if (!sp_slab_quar_on || sp_slab_on <= 0) return;
    void *m = mmap(NULL, sp_slab_cap / SP_SLAB_CHUNK * SP_SLAB_NW * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { sp_slab_quar_on = 0; return; }
    sp_slab_quar = (uint64_t *)m;
  }
  if (sp_slab_quar_bytes <= SP_SLAB_QUAR_MAX) return;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_bm *bm = sp_slab_bm_of(ch);
      uint64_t *q = sp_slab_quar_of(ch), any = 0;
      for (unsigned w = 0; w < (ch->nslots + 63u) >> 6; w++) {
        if (!q[w]) continue;
        bm_and(&bm->pin[w], ~q[w]); any |= q[w]; q[w] = 0;
      }
      if (any) sp_slab_avail_push(ch);
    }
  }
  sp_slab_quar_bytes = 0;
}
void sp_slab_epoch_flip(void) {
  sp_slab_runs_release();
  sp_slab_epoch = sp_slab_epoch + 1;   /* under the barrier: nobody allocates or sweeps meanwhile */
}
/* Every chunk one worker owns, in one pass over the bitmaps. The epoch to
   reclaim from is the one before the current; `full` frees the old
   generation too; `aging` carries young survivors the mark did not promote
   into the current epoch. What dies with a finalizer bit set has its
   finalizer (or recycler) run, from this thread. The caller says which
   thread is sweeping: an owner beside the program, a sweeper thread, or the
   collector under the barrier -- the bit operations are the same. */
void sp_slab_sweep_worker(int wid, int full, int aging, int (*die)(void *hdr), sp_slab_sweep_stats *st) {
  sp_slab_sweep_stats acc; memset(&acc, 0, sizeof acc);
  if (sp_slab_on <= 0 || wid < 0 || wid >= SP_SLAB_NWK) { if (st) *st = acc; return; }
  sp_slab_worker *wk = &sp_slab_wk[wid];
  unsigned e = sp_slab_epoch & 1, pe = e ^ 1;
  SP_ATOMIC_STORE(&wk->sweeping, 1, __ATOMIC_RELEASE);
  for (sp_slab_chunk *ch = SP_ATOMIC_LOAD(&wk->owned, __ATOMIC_ACQUIRE); ch; ch = ch->own_next) {
    if (!ch->in_use) continue;
    sp_slab_bm *bm = sp_slab_bm_of(ch);
    unsigned nw = (ch->nslots + 63) >> 6;
    unsigned csize = sp_slab_csize[ch->cls];
    size_t freed = 0;
    for (unsigned w = 0; w < nw; w++) {
      uint64_t yv = bm_load(&bm->young[pe][w]);
      uint64_t ov = bm_load(&bm->old[w]);
      uint64_t mv = bm_load(&bm->mark[w]);
      uint64_t pv = bm_load(&bm->pin[w]);
      /* the headers a pool holds (pinned, in no generation, with their
         finalizer bit) are resident like any live object: the object budget
         is sized from them too, as it was when the lists counted them */
      if (pv) {
        uint64_t parked_now = pv & bm_load(&bm->fin[w]) & ~(yv | ov | bm_load(&bm->young[e][w]));
        if (parked_now) acc.parked += (size_t)SP_POPCOUNT64(parked_now) * csize;
      }
      /* a word with nothing to reclaim is skipped, unless it carries marks:
         a minor's mark reaches old strings too, and a mark left behind would
         read as "already marked" next cycle and keep a dead string */
      if (!yv && !mv && !(full && ov)) continue;
      uint64_t dead = yv & ~mv & ~ov & ~pv;
      if (full) dead |= ov & ~mv & ~pv;
      if (aging) {
        uint64_t carry = yv & mv & ~ov;
        if (carry) { bm_or(&bm->young[e][w], carry); acc.kept_young += (size_t)SP_POPCOUNT64(carry) * csize; }
      }
      /* a dead object with a finalizer or a recycler: the callback runs it
         and says whether the slot is freed or kept. A pooled header stays
         allocated: the callback pins it (sp_slab_park) BEFORE handing it to
         its pool, since the pool may hand it out again on another thread at
         once, and that thread's relive undoes the pin. So nothing here
         writes a kept slot's bits after the callback: it is either still
         pinned or already alive again, and either way it is not dead. */
      uint64_t fdead = dead & bm_load(&bm->fin[w]);
      uint64_t parked = 0;
      if (fdead) {
        /* what the pass costs per slot is the finalizers it runs: the rest is
           a few words per chunk (the object budget's mark-share gate reads
           `swept` as the sweep's per-slot work, sp_gc_retune_object) */
        acc.swept += (size_t)SP_POPCOUNT64(fdead);
        /* pinned as a word before any callback runs, unpinned as a word for
           those the callbacks freed: a slot the recycler kept is pinned by
           then, whatever its pool did with the header meanwhile */
        bm_or(&bm->pin[w], fdead);
        uint64_t left = fdead, freed_now = 0;
        while (left) {
          unsigned i = (unsigned)SP_CTZ64(left);
          left &= left - 1;
          if (die(sp_slab_chunk_base(ch) + (size_t)((w << 6) + i) * csize)) freed_now |= (uint64_t)1 << i;
        }
        if (freed_now) bm_and(&bm->pin[w], ~freed_now);
        parked = fdead & ~freed_now;
      }
      if (parked) {
        dead &= ~parked;
        acc.parked += (size_t)SP_POPCOUNT64(parked) * csize;   /* this cycle's, beside the ones counted above */
        if (sp_slab_verify_on) { uint64_t v = parked; while (v) { unsigned b = (unsigned)SP_CTZ64(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 8 + 10 * (int)pe); } }
      }
      if (sp_slab_verify_on) { uint64_t v = yv & ~dead; while (v) { unsigned b = (unsigned)SP_CTZ64(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 30 + (int)pe); } }
      if (dead) {
        uint64_t sd = dead & bm_load(&bm->str[w]);
        acc.freed_str += (size_t)SP_POPCOUNT64(sd) * csize;
        acc.freed_obj += (size_t)SP_POPCOUNT64(dead & ~sd) * csize;
        if (fdead | (dead & bm_load(&bm->fin[w]))) bm_and(&bm->fin[w], ~dead);
        if (sd) bm_and(&bm->str[w], ~dead);
        if (full && (ov & dead)) bm_and(&bm->old[w], ~dead);
        freed += (size_t)SP_POPCOUNT64(dead);
        if (sp_slab_verify_on) { uint64_t v = dead; while (v) { unsigned b = (unsigned)SP_CTZ64(v); v &= v - 1; sp_slab_note(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, 7); } }
        if (SP_EXPECT(sp_slab_quar != NULL, 0)) sp_slab_quarantine(ch, bm, w, dead, sd, 0);
      }
      /* the epoch's word is spent: dead, promoted or carried, every bit is
         accounted for. So are the marks: the next cycle starts clean. */
      bm_store(&bm->young[pe][w], 0);
      bm_store(&bm->mark[w], 0);
    }
    acc.slots += ch->nslots;
    if (freed) { acc.freed_slots += freed; sp_slab_avail_push(ch); }
    if (sp_slab_verify_on) sp_slab_verify_chunk(ch, "after its sweep");
  }
  SP_ATOMIC_STORE(&wk->sweeping, 0, __ATOMIC_RELEASE);
  if (st) *st = acc;
}
/* The verifiers walk every allocated slot of every chunk: objects (not
   strings) whose generation is young, old, or either. */
void sp_slab_each_object(int young, int old, void (*fn)(void *hdr, void *arg), void *arg) {
  if (sp_slab_on <= 0) return;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_bm *bm = sp_slab_bm_of(ch);
      unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
      for (unsigned w = 0; w < nw; w++) {
        uint64_t v = 0;
        if (young) v |= bm->young[0][w] | bm->young[1][w];
        if (old) v |= bm->old[w];
        v &= ~bm->str[w];   /* not strings; a payload or a parked header is pinned and in no generation */
        while (v) {
          unsigned b = (unsigned)SP_CTZ64(v); v &= v - 1;
          fn(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, arg);
        }
      }
    }
  }
}
/* the same over heap strings (the slot's start; the body follows the header) */
void sp_slab_each_string(int young, int old, void (*fn)(void *hdr, void *arg), void *arg) {
  if (sp_slab_on <= 0) return;
  for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
    sp_slab_arena *ar = (sp_slab_arena *)a;
    for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
      sp_slab_chunk *ch = &ar->ch[i];
      if (!ch->in_use) continue;
      sp_slab_bm *bm = sp_slab_bm_of(ch);
      unsigned nw = (ch->nslots + 63) >> 6, csize = sp_slab_csize[ch->cls];
      for (unsigned w = 0; w < nw; w++) {
        uint64_t v = 0;
        if (young) v |= bm->young[0][w] | bm->young[1][w];
        if (old) v |= bm->old[w];
        v &= bm->str[w];
        while (v) {
          unsigned b = (unsigned)SP_CTZ64(v); v &= v - 1;
          fn(sp_slab_chunk_base(ch) + (size_t)((w << 6) + b) * csize, arg);
        }
      }
    }
  }
}

/* ---- release: fully free chunks back to the OS ---- */
/* The available lists are rebuilt in ADDRESS order. A chunk joins the list
   whenever a sweep frees its first slot, in whatever order the sweep meets
   them, and after a few cycles consecutive refills of one class hopped
   across the heap: a 72 MB tree benchmark (gcbench) ran 30% slower on
   16 KB chunks than on glibc, whose coalescing hands back one run of
   addresses, and 15% faster once the refills walked the chunks in order. */
static int sp_slab_chunk_cmp(const void *a, const void *b) {
  uintptr_t x = (uintptr_t)*(sp_slab_chunk *const *)a, y = (uintptr_t)*(sp_slab_chunk *const *)b;
  return x < y ? -1 : x > y;
}
/* The sort buffer is per thread: the owners release their own lists beside
   the program, at the same time, and a shared buffer would need a lock every
   one of them queued on. */
static SP_TLS sp_slab_chunk **sp_slab_sortbuf = NULL;
static SP_TLS size_t sp_slab_sortcap = 0;
static sp_slab_chunk *sp_slab_sort_avail(sp_slab_chunk *head) {
  size_t n = 0;
  for (sp_slab_chunk *ch = head; ch; ch = ch->next_avail) {
    if (n == sp_slab_sortcap) {
      size_t c = sp_slab_sortcap ? sp_slab_sortcap * 2 : 256;
      sp_slab_chunk **nb = (sp_slab_chunk **)realloc(sp_slab_sortbuf, c * sizeof *nb);
      if (!nb) return head;   /* unsorted is still correct */
      sp_slab_sortbuf = nb; sp_slab_sortcap = c;
    }
    sp_slab_sortbuf[n++] = ch;
  }
  if (n < 2) return head;
  qsort(sp_slab_sortbuf, n, sizeof *sp_slab_sortbuf, sp_slab_chunk_cmp);
  for (size_t i = 0; i + 1 < n; i++) sp_slab_sortbuf[i]->next_avail = sp_slab_sortbuf[i + 1];
  sp_slab_sortbuf[n - 1]->next_avail = NULL;
  return sp_slab_sortbuf[0];
}
static inline int sp_slab_chunk_empty(sp_slab_chunk *ch) {
  sp_slab_bm *bm = sp_slab_bm_of(ch);
  unsigned nw = (ch->nslots + 63) >> 6;
  for (unsigned w = 0; w < nw; w++)
    if (bm_load(&bm->young[0][w]) | bm_load(&bm->young[1][w]) | bm_load(&bm->old[w]) | bm_load(&bm->pin[w])) return 0;
  return 1;
}

/* SPINEL_GC_PHASES: what a release spends its time on -- chunks walked,
   chunks handed back (each one an madvise), and the madvise time itself. */
unsigned long long sp_slab_rel_calls = 0, sp_slab_rel_walked = 0, sp_slab_rel_madv = 0;
double sp_slab_rel_madv_t = 0, sp_slab_rel_sort_t = 0;
static double sp_slab_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* One worker's lists, released by their OWNER (or, for a slot no worker
   runs, by the collector under the barrier), right after its own sweep: the
   walk, the sort and the madvise calls that the collector used to do for
   every worker in turn under the barrier are spread over the workers
   instead, each on its own lists, while the others run. Only the owner pops
   its available lists, so detaching a list whole (an exchange with NULL)
   makes it private for the walk; a sweeper thread pushing a chunk meanwhile
   lands it on the fresh head, and the kept chunks are merged back under it
   with the same compare-and-swap the sweepers push with. A fully free chunk
   is stable once seen: every slot is out of every generation, so nothing can
   free into it, and only its owner could allocate from it. Never the chunk
   the class allocates from, and never while a sweep of this worker's chunks
   is still running on another thread: that sweep clears bits of the very
   chunks the pool would carve for someone else. The empty pool is shared,
   so it takes the slab lock. */
void sp_slab_release_worker(int wid) {
  if (sp_slab_on <= 0 || wid < 0 || wid >= SP_SLAB_NWK) return;
  sp_slab_worker *wk = &sp_slab_wk[wid];
  if (SP_ATOMIC_LOAD(&wk->sweeping, __ATOMIC_ACQUIRE)) return;
  wk->taken = 0;
  if (sp_gc_ph_on) sp_slab_rel_calls++;
  for (int cls = 0; cls < SP_SLAB_NCLS; cls++) {
    /* The reserve is per class: what the class took this cycle it will
       take again next cycle, and a reserve shared across the classes was
       spent by the first few, so the rest handed their empties back and
       carved fresh ones every cycle -- 37,000 chunks a second on a server,
       each handed to the kernel and faulted in again. */
    long reserve = wk->taken_cls[cls];
    wk->taken_cls[cls] = 0;
    if (reserve < SP_SLAB_RESERVE) reserve = SP_SLAB_RESERVE;
    sp_slab_chunk *ch = (sp_slab_chunk *)(uintptr_t)SP_ATOMIC_EXCHANGE(&wk->avail[cls], NULL, __ATOMIC_ACQ_REL);
    if (!ch) continue;
    sp_slab_chunk *keep = NULL, *give = NULL;
    while (ch) {
      sp_slab_chunk *nx = ch->next_avail;
      if (sp_gc_ph_on) sp_slab_rel_walked++;
      int empty = sp_slab_chunk_empty(ch);
      if (empty && reserve <= 0 && ch != SP_ATOMIC_LOAD(&wk->cur[cls], __ATOMIC_RELAXED)) {
        ch->next_avail = give; give = ch;   /* handed back below, in address order */
      }
      else {
        if (empty) reserve--;
        ch->next_avail = keep; keep = ch;
      }
      ch = nx;
    }
    if (give) {
      /* the chunks handed back, in address order, so neighbours go to the
         kernel as one madvise: a call per 16 KB chunk was most of the
         release's time, and half of them sit next to each other */
      give = sp_slab_sort_avail(give);
      sp_slab_chunk *gt = give;
      while (gt) {
        char *lo = sp_slab_chunk_base(gt), *hi = lo + SP_SLAB_CHUNK;
        int touched = gt->touched;
        sp_slab_chunk *run_end = gt;
        while (run_end->next_avail && sp_slab_chunk_base(run_end->next_avail) == hi &&
               run_end->next_avail->touched == touched) {
          run_end = run_end->next_avail; hi += SP_SLAB_CHUNK;
        }
        if (touched) {
          double mt0 = sp_gc_ph_on ? sp_slab_now() : 0;
          madvise(lo, (size_t)(hi - lo), MADV_DONTNEED);
          if (sp_gc_ph_on) { sp_slab_rel_madv++; sp_slab_rel_madv_t += sp_slab_now() - mt0; }
        }
        sp_slab_chunk *nx = run_end->next_avail;
        /* on_avail stays SET on a chunk handed back: a sweeper whose free
           landed before the walk read the chunk as empty can still be short
           of its own "put it on the list" step, and that step's exchange
           must find the chunk spoken for, or the owner would pop a chunk the
           pool has since carved for someone else. The carve (sp_slab_refill)
           is what clears it. Off the owned list here, before the pool can
           hand it to a worker that links it into its own. */
        for (sp_slab_chunk *c2 = gt;; c2 = c2->next_avail) {
          if (c2->own_prev) c2->own_prev->own_next = c2->own_next;
          else wk->owned = c2->own_next;
          if (c2->own_next) c2->own_next->own_prev = c2->own_prev;
          c2->own_next = c2->own_prev = NULL;
          memset(sp_slab_bm_of(c2), 0, sizeof(sp_slab_bm));
          c2->in_use = 0; c2->on_avail = 1; c2->touched = 0;
          c2->nslots = 0; c2->hint = 0;
          if (c2 == run_end) break;
        }
        gt = nx;
      }
      gt = give; while (gt->next_avail) gt = gt->next_avail;
      SP_SLAB_LOCK();
      gt->next_avail = sp_slab_empty; sp_slab_empty = give;
      SP_SLAB_UNLOCK();
    }
    if (keep) {
      double st0 = sp_gc_ph_on ? sp_slab_now() : 0;
      keep = sp_slab_sort_avail(keep);
      if (sp_gc_ph_on) sp_slab_rel_sort_t += sp_slab_now() - st0;
      sp_slab_chunk *kt = keep; while (kt->next_avail) kt = kt->next_avail;
#ifdef SP_THREADS
      sp_slab_chunk *oh;
      do { oh = SP_ATOMIC_LOAD(&wk->avail[cls], __ATOMIC_ACQUIRE); kt->next_avail = oh;
      } while (!SP_ATOMIC_CAS(&wk->avail[cls], &oh, keep, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
#else
      kt->next_avail = wk->avail[cls]; wk->avail[cls] = keep;
#endif
    }
  }
}
/* The collector's part once the owners release their own: the slots no
   active worker owns (a worker that has exited, the sweeper slot), which
   nobody else will walk. Under the barrier. */
void sp_slab_release_from(int first) {
  if (sp_slab_on <= 0) return;
  for (int w = first; w < SP_SLAB_NWK; w++) {
    sp_slab_worker *wk = &sp_slab_wk[w];
    for (int cls = 0; cls < SP_SLAB_NCLS; cls++) {
      if (!wk->avail[cls]) continue;
      sp_slab_release_worker(w);
      break;
    }
  }
}
/* every worker's, from one thread with nothing else running (the
   single-threaded build, or a stop-the-world sweep) */
void sp_slab_release(void) {
  if (sp_slab_on <= 0) return;
  /* SPINEL_GC_PHASES: the slab's footprint every 64th release -- chunks in
     use, of which fully free (the reserve), and the bytes their live slots
     hold -- so the resident set can be read against what is live. */
  if (sp_gc_ph_on) {
    static int tick = 0;
    if ((++tick & 63) == 0) {
      size_t nuse = 0, nempty = 0, live = 0, untouched = 0;
      for (uintptr_t a = sp_slab_base; a < sp_slab_brk; a += SP_SLAB_ARENA) {
        sp_slab_arena *ar = (sp_slab_arena *)a;
        for (int i = SP_SLAB_FIRST; i < (int)SP_SLAB_NCHUNK; i++) {
          sp_slab_chunk *ch = &ar->ch[i];
          if (!ch->in_use) { if (!ch->touched) untouched++; continue; }
          nuse++;
          sp_slab_bm *bm = sp_slab_bm_of(ch);
          size_t used = 0;
          for (unsigned w = 0; w < (ch->nslots + 63u) / 64u; w++)
            used += (size_t)SP_POPCOUNT64(bm->young[0][w] | bm->young[1][w] | bm->old[w] | bm->pin[w]);
          if (!used) nempty++;
          live += used * sp_slab_csize[ch->cls];
        }
      }
      fprintf(stderr, "[slab] arenas %zu  chunks in use %zu (fully free %zu)  live in slots %.1f MB  resident chunks %.1f MB\n",
              (size_t)((sp_slab_brk - sp_slab_base) / SP_SLAB_ARENA), nuse, nempty,
              live / 1048576.0, nuse * (SP_SLAB_CHUNK / 1048576.0));
    }
  }
  for (int w = 0; w < SP_SLAB_NWK; w++) sp_slab_release_worker(w);
}
