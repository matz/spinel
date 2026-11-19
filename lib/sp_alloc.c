/* Shared definitions backing sp_alloc.h for the runtime and generated unit. */
#include <time.h>
#include <signal.h>    /* SPINEL_ALLOC_REPORT_SIGNAL: dump without exiting */
#include <strings.h>   /* strcasecmp (that signal named rather than numbered) */
#include <unistd.h>    /* write/read: the handler's only safe way to say "dump" */
#include <fcntl.h>     /* the wake pipe's O_NONBLOCK and FD_CLOEXEC */
#include <errno.h>     /* EINTR on the reader's park */
#ifdef SP_THREADS
#include <pthread.h>   /* the thread that does the writing */
#endif
#include "sp_alloc.h"
#include "sp_dtoa.h"   /* sp_format_float for locale-independent Float#to_s */
/* Per-site allocation attribution (SPINEL_ALLOC_SITES=1, on top of
   SPINEL_ALLOC_REPORT). The site is the raw return address of the frame that
   asked for the allocation -- captured here, symbolised only at dump time, so
   nothing allocates on the counted path. execinfo is optional; without it the
   report stays per-type. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define SP_ALLOC_SITE_AVAILABLE 1
#  endif
#endif
#ifndef SP_ALLOC_SITE_AVAILABLE
#  define SP_ALLOC_SITE_AVAILABLE 0
#endif

/* Mark-share reporting mode, read with the other GC controls at startup. */
int sp_gc_obj_budget_mode = 2;
/* The last gate decision, in 1024ths, so the stats line can report it and a
   test can read it. 1024 is `walk`, 0 is `obj`. */
size_t sp_gc_obj_alpha1024 = 1024;
/* Hold the object heap's collection budget at its floor. */
int sp_gc_obj_budget_fixed = 0;

const char sp_str_empty_data[] = "\xff";
/* nil.to_s, true.to_s and false.to_s answer FROZEN Strings in CRuby: a
   header-carrying static object marked 0xf1, as a frozen literal is */
/* ...each one String object, as a generated literal is (sp_StringLiteral:
   the header links to the static handle, tagged), so each of them is one
   object wherever it is boxed or held, as CRuby's is */
static sp_StringLiteral sp_fz_empty_l, sp_fz_true_l, sp_fz_false_l;
#define SP_FZ_LINK(l) ((sp_str_hdr *)((char *)&(l).s + 1))
/* the cached Symbol spelling's header link (sp_str_is_sym_name) */
const sp_str_hdr sp_sym_str_tag = { NULL, 0, 0, 0 };
static struct { sp_str_hdr h; unsigned char m; char d[1]; } sp_fz_empty_s = { { SP_FZ_LINK(sp_fz_empty_l), 1 | SP_STR_SIZE_ASCII7, 0, 0 }, 0xf1, "" };
static struct { sp_str_hdr h; unsigned char m; char d[5]; } sp_fz_true_s = { { SP_FZ_LINK(sp_fz_true_l), 5 | SP_STR_SIZE_ASCII7, 4, 0 }, 0xf1, "true" };
static struct { sp_str_hdr h; unsigned char m; char d[6]; } sp_fz_false_s = { { SP_FZ_LINK(sp_fz_false_l), 6 | SP_STR_SIZE_ASCII7, 5, 0 }, 0xf1, "false" };
static sp_StringLiteral sp_fz_empty_l = SP_STRING_LITERAL_INIT(sp_fz_empty_l, sp_fz_empty_s.d, 0);
static sp_StringLiteral sp_fz_true_l = SP_STRING_LITERAL_INIT(sp_fz_true_l, sp_fz_true_s.d, 4);
static sp_StringLiteral sp_fz_false_l = SP_STRING_LITERAL_INIT(sp_fz_false_l, sp_fz_false_s.d, 5);
const char *const sp_str_frozen_empty = sp_fz_empty_s.d;
const char *const sp_str_frozen_true = sp_fz_true_s.d;
const char *const sp_str_frozen_false = sp_fz_false_s.d;

SP_TLS int sp_ffi_bin_len = 0;   /* see sp_alloc.h: byte count for :binstr / :cbinstr */

/* Object-heap collection threshold (was per-TU static in spinel_rt.h; now
   shared so sp_gc_alloc can live in sp_alloc.h and lib TUs allocate too). */
size_t sp_gc_threshold = 256 * 1024;
size_t sp_gc_threshold_init = 256 * 1024;
int sp_gc_stress_checked = 0;
/* Stress pins the threshold instead of merely seeding it: the retunes float
   the trigger to live*4 with the base as a FLOOR, so on any program whose
   live set outgrows the base, stress stopped stressing after the first
   collection -- request-time bugs sat behind a cadence identical to the
   default's while boot-time ones reproduced instantly (#3513). */
int sp_gc_stress_pin = 0;

#ifdef SP_THREADS
pthread_mutex_t sp_heap_lock = PTHREAD_MUTEX_INITIALIZER;   /* see sp_alloc.h */

/* One-time SPINEL_GC_STRESS check, run single-threaded before the first helper
   worker spawns (sp_sched_ensure_workers). The alloc fast paths keep their lazy
   `if (!checked)` guard for the single-threaded build, but under threads letting
   workers race to first-write that flag on the hot path is a data race; doing it
   here once means every worker only ever reads it (the pthread_create of the
   helpers is the happens-before edge). Idempotent: safe if main already tripped
   the lazy guard during startup. */
void sp_alloc_stress_init(void) {
  const char *e = getenv("SPINEL_GC_STRESS");
  int stress = (e && *e && *e != '0');
  if (!sp_gc_stress_checked) {
    sp_gc_stress_checked = 1;
    if (stress) { SP_GC_CTR_SET(sp_gc_threshold, 2048); sp_gc_threshold_init = 2048; sp_gc_stress_pin = 1; }
  }
}

/* Size the collection budget for the worker count, once, before any helper
   spawns (same single-threaded window as the stress check above).

   The object-heap trigger compares the GLOBAL live-byte total against one
   threshold, so N workers cross it N times faster in wall clock -- and every
   crossing now stops N workers instead of one. Measured on an
   allocation-heavy program: the collection COUNT is flat across worker counts
   (the total allocated is what it is), but 8 workers ran 1.7x slower than 1
   while burning 2.9x the CPU, all of it in park/mark/unpark. Scale the
   budget with the number of workers to keep the cost per worker bounded.

   The cost is bounded and small: N * 256 KB of garbage retained between
   collections, 2 MB at eight workers. SPINEL_GC_THRESHOLD_KB overrides the
   base for a program that wants to trade more memory for fewer stops.

   Not scaled under GC stress: that mode exists to maximize collections, and
   multiplying its 2 KB budget would quietly weaken every stress run. */
#endif  /* SP_THREADS -- the floors below are read by EVERY program */
static void sp_alloc_floor_from_env(const char *name) {
  const char *e = getenv(name);
  if (!e || !*e) return;
  long v = atol(e);
  if (v <= 0) return;
  size_t base = (size_t)v * 1024;
  SP_GC_CTR_SET(sp_gc_threshold, base);
  sp_gc_threshold_init = base;
}
/* Start with the generic floor and allow the object-specific name to
   override it. Read again before scaling for the worker pool. */
void sp_alloc_floors_from_env(void) {
  sp_alloc_floor_from_env("SPINEL_GC_THRESHOLD_KB");
  sp_alloc_floor_from_env("SPINEL_GC_THRESHOLD_OBJ_KB");
  /* Level 2 overrides either floor and the allocator's one-shot check. */
  { const char *st = getenv("SPINEL_GC_STRESS");
    if (st && atoi(st) >= 2) {
      SP_GC_CTR_SET(sp_gc_threshold, 0); sp_gc_threshold_init = 0;
      sp_gc_stress_pin = 1; sp_gc_stress_checked = 1;
    } }
}
#ifdef SP_THREADS
void sp_alloc_worker_tune(int workers) {
  sp_alloc_floors_from_env();
  {
    const char *st = getenv("SPINEL_GC_STRESS");
    if (st && *st && *st != '0') return;
  }
  if (workers < 1) workers = 1;
  if (workers > SP_MAX_WORKERS) workers = SP_MAX_WORKERS;
  if (workers == 1) return;
  sp_gc_threshold_init *= (size_t)workers;
  /* Raise the CURRENT threshold to the new base, do not multiply it. The cost
     budgeted above -- N * 256 KB retained between collections -- is what the
     multiply costs when it lands on the base, which is where it lands for a
     program that creates its threads before doing any work. A program that
     creates its first thread after its heap has grown was handed N * whatever
     the adaptive threshold had become: 76 MB -> 2.4 GB on a 32-core machine,
     with no collection involved, and the churn that followed then ran to a
     997 MB heap against a 66 MB live set without collecting once. The
     multiplier is the configured worker limit. The number of live Ruby
     threads can differ from it (#4146). */
  { size_t cur = SP_GC_CTR_GET(sp_gc_threshold);
    if (sp_gc_threshold_init > cur) SP_GC_CTR_SET(sp_gc_threshold, sp_gc_threshold_init); }
}
#endif

/* size_t multiply that stops at the top instead of wrapping. A threshold that
   wraps is not a large threshold, it is an OFF switch: `bytes >= threshold`
   never fires again, nothing collects, and the retune that would correct it is
   only reached by a collection (#4073). */
static size_t sp_gc_sat_mul(size_t v, size_t k) {
  return (k && v > (size_t)-1 / k) ? (size_t)-1 : v * k;
}
/* SPINEL_GC_STATS=1: a line on stderr, at most once a second, saying how many
   collections have run and what they cost. A server whose GC share of CPU
   climbs with concurrency and one that simply collects more often are the same
   picture from a profile; separating them needs the COUNT beside the total
   time, and spinel exposed neither (#4352). Reported from the object retune
   because that runs at the end of every collection. */
static void sp_gc_stats_emit(void);
static void sp_gc_stats_report(void) {
  static int on = -1;
  static double last = 0;
  if (on < 0) {
    const char *e = getenv("SPINEL_GC_STATS"); on = (e && *e && *e != '0') ? 1 : 0;
    /* SPINEL_GC_PHASES arms the same reporter on its own, so the breakdown does
       not also require SPINEL_GC_STATS to be set. */
    if (sp_gc_ph_on) on = 1;
    /* A program that exits before the next tick would otherwise report nothing
       but its first collection, so the totals are also printed on the way out.
       A server is killed rather than returning from main, which is why the
       periodic line exists at all. */
    if (on) atexit(sp_gc_stats_emit);
  }
  if (!on) return;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  if (now - last < 1.0) return;
  last = now;
  sp_gc_stats_emit();
}
/* The `[gc]` line itself, and under SPINEL_GC_PHASES the `[gcph]` breakdown
   below it. */
static void sp_gc_stats_emit(void) {
  static double first = 0;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  if (first == 0) first = now;
  double wall = now - first;
  unsigned long long n = sp_gc_stat_collections;
  fprintf(stderr,
          "[gc] %llu collections (%llu full) in %.2fs of %.1fs wall (%.1f%%), %.2fms avg; "
          "live %.1f MB obj; trigger %.1f MB obj; mark share %.2f\n",
          n, sp_gc_stat_fulls, sp_gc_stat_seconds, wall,
          wall > 0 ? 100.0 * sp_gc_stat_seconds / wall : 0.0,
          n ? 1000.0 * sp_gc_stat_seconds / (double)n : 0.0,
          (double)SP_GC_CTR_GET(sp_gc_bytes) / 1048576.0,
          (double)SP_GC_CTR_GET(sp_gc_threshold) / 1048576.0,
          (double)sp_gc_obj_alpha1024 / 1024.0);
  if (!sp_gc_ph_on) return;
  fprintf(stderr,
          "[gcph] marked %llu objs  swept %llu slots\n",
          (unsigned long long)SP_GC_CTR_GET(sp_gc_ct_marked), (unsigned long long)SP_GC_CTR_GET(sp_gc_ct_swept));
  fprintf(stderr,
          "[gcph] mark %.3fs  old sweep %.3fs  slot sweep %.3fs (longest task %.3fs; tasks total %.3fs, obj lists %.3fs)  "
          "remembered clear %.3fs  trim %.3fs  of %.3fs total\n",
          sp_gc_ph_mark, sp_gc_ph_oldsweep, sp_gc_ph_slotsweep, sp_gc_ph_slot_max,
          sp_gc_ph_task_sum, sp_gc_ph_task_obj,
          sp_gc_ph_rembclear, sp_gc_ph_trim,
          sp_gc_stat_seconds);
  { extern double sp_gc_ph_park_sweeping; extern unsigned long long sp_gc_ph_park_sweeping_n;
    if (sp_gc_ph_park_sweeping_n > 0)
      fprintf(stderr, "[gcph] park wait: %.3fs of it on %llu barriers raised while an owner sweep of the previous cycle was still running\n",
              sp_gc_ph_park_sweeping, sp_gc_ph_park_sweeping_n); }
  if (sp_gc_ph_barrier > 0)
    fprintf(stderr, "[gcph] barrier: %.3fs from stop to release, of which %.3fs waiting for the workers to park; parallel mark on %llu drains, %.1f helpers each\n",
            sp_gc_ph_barrier, sp_gc_ph_park, sp_gc_ph_mk_drains,
            sp_gc_ph_mk_drains ? (double)sp_gc_ph_mk_helpers / (double)sp_gc_ph_mk_drains : 0.0);
  { extern unsigned long long sp_gc_ph_mk_by_helpers, sp_gc_ph_mk_spills, sp_gc_ph_mk_takes;
    if (sp_gc_ph_mk_drains)
      fprintf(stderr, "[gcph] parallel mark: %llu of %llu objects marked by helpers; %llu chunks spilled, %llu taken; collector drain %.3fs (idle in it %.3fs) join %.3fs\n",
              sp_gc_ph_mk_by_helpers, (unsigned long long)SP_GC_CTR_GET(sp_gc_ct_marked), sp_gc_ph_mk_spills, sp_gc_ph_mk_takes,
              sp_gc_ph_mk_drain, sp_gc_ph_mk_idle, sp_gc_ph_mk_join); }
  if (sp_gc_ph_conc_wall > 0)
    fprintf(stderr, "[gcph] concurrent sweep: %.3fs wall beside the program (longest task %.3fs); %llu collections waited %.3fs for the previous sweep (not in the total above)\n",
            sp_gc_ph_conc_wall, sp_gc_ph_slot_max, sp_gc_ph_conc_waits, sp_gc_ph_conc_wait);
  if (sp_gc_ph_conc_wall > 0)
    { extern double sp_gc_ph_wait_top;
      fprintf(stderr, "[gcph] concurrent sweep, applied under the barrier: objects %.3fs  slab release %.3fs  (the join at the top of the collection: %.3fs)\n",
              sp_gc_ph_apply_obj, sp_gc_ph_apply_release, sp_gc_ph_wait_top); }
  { extern unsigned long long sp_gc_ph_trim_req, sp_gc_ph_trim_inline; extern double sp_gc_ph_trim_inline_t;
    if (sp_gc_ph_trim_req > 0)
      fprintf(stderr, "[gcph] trim: %llu requests, %llu run inline (%.3fs), trimmer thread %s\n",
              sp_gc_ph_trim_req, sp_gc_ph_trim_inline, sp_gc_ph_trim_inline_t, sp_gc_trimmer_on ? "on" : "off"); }
  { extern unsigned long long sp_slab_rel_calls, sp_slab_rel_walked, sp_slab_rel_madv; extern double sp_slab_rel_madv_t, sp_slab_rel_sort_t;
    if (sp_slab_rel_calls > 0)
      fprintf(stderr, "[gcph] slab release: %llu releases walked %llu chunks, handed back %llu (madvise %.3fs, sort %.3fs)\n",
              sp_slab_rel_calls, sp_slab_rel_walked, sp_slab_rel_madv, sp_slab_rel_madv_t, sp_slab_rel_sort_t); }
  /* The mark, one level down, because "mark grew" has two causes that want
     different answers: more ROOTS to scan and more GRAPH to trace. `fibers` is
     every live fiber's saved roots, walked serially, and it grows with the
     number of in-flight fibers rather than with the worker count; `scan` is the
     trace that drains what the roots found, and it grows because those fibers
     hold live objects. Only the first is what handing the fiber list to the
     parked workers would address (#4384). The four sum to `mark` above. */
  fprintf(stderr,
          "[gcph] mark: roots %.3fs  fibers %.3fs  globals %.3fs  scan %.3fs\n",
          sp_gc_ph_mk_roots, sp_gc_ph_mk_fibers,
          sp_gc_ph_mk_globals, sp_gc_ph_mk_scan);
}

void sp_gc_retune_object(size_t before) {
  sp_gc_stats_report();
  if (sp_gc_stress_pin || sp_gc_obj_budget_fixed) { sp_gc_threshold = sp_gc_threshold_init; return; }
  size_t live = sp_gc_bytes;
  /* Report the mark's share using the object and slot counts. */
  static size_t prev_marked = 0, prev_swept = 0;
  size_t cmk = SP_GC_CTR_GET(sp_gc_ct_marked), csw = SP_GC_CTR_GET(sp_gc_ct_swept);
  size_t marked = cmk > prev_marked ? cmk - prev_marked : 0;
  size_t swept  = csw > prev_swept  ? csw - prev_swept  : 0;
  prev_marked = cmk; prev_swept = csw;
  size_t alpha = 1024;
  {
    int nw = 1;
#ifdef SP_THREADS
    nw = sp_active_workers; if (nw < 1) nw = 1;
#endif
    size_t k = (nw > 1) ? 4u : 64u;
    if (marked || swept) {
      size_t num = k * marked, den = num + swept;
      alpha = den ? (num * 1024) / den : 1024;
      if (alpha > 1024) alpha = 1024;
    }
  }
  if (sp_gc_obj_budget_mode == 0) alpha = 0;
  else if (sp_gc_obj_budget_mode == 1) alpha = 1024;
  sp_gc_obj_alpha1024 = alpha;
  /* saturating: the live counter is a heuristic and is allowed to lag, so it
     can read above the pre-collect total. Wrapping made `freed` enormous, the
     productive-sweep test went false, and the threshold was taken from a live
     count that had itself wrapped. */
  size_t freed = before > live ? before - live : 0;

  size_t next = sp_gc_sat_mul(freed < before / 4 ? before : live, 2);
  sp_gc_threshold = next < sp_gc_threshold_init ? sp_gc_threshold_init : next;
}
/* The collector the allocation triggers call. A program built with
   --gc=none defines this pointer itself, as NULL: the definition here, being
   weak, gives way, nothing then names the collector, and its mark, sweep
   and verifiers leave the link (--gc-sections); the triggers are lifted the
   first time one fires, so no allocation asks again. */
#ifdef SP_GC_NONE
/* The archive built for --gc=none (lib/libspinel_rt_nogc.a and its
   variants, -DSP_GC_NONE) names the collector nowhere: this definition
   agrees with the program's, which is the strong one. */
SP_WEAK void (*sp_gc_collect_fn)(void) = NULL;
#elif defined(_WIN32)
/* Not weak on Windows. A COFF weak definition is a weak external whose
   default lives in its own section, and MinGW ld's --gc-sections drops that
   section (the references name the weak external, not the section): the
   pointer was left undefined, its 0 relocated by the ASLR delta into a
   non-NULL garbage address, and the first collection faulted on it -- under
   the runtime's SIGSEGV emulation, the same fault over and over, a hang.
   Nothing needs it weak here: a --gc=none program links the _nogc archive
   (the branch above), and the C extension archives that a --gc=none
   program would share are not built for Windows. */
void (*sp_gc_collect_fn)(void) = sp_gc_collect;
#else
SP_WEAK void (*sp_gc_collect_fn)(void) = sp_gc_collect;
#endif
/* Set by GC.start for the next collection: a full one, whatever the cadence
   (sp_gc_collect clears it). Here, not in sp_gc.c, so the --gc=none archive
   that names no collector still links GC.start. */
int sp_gc_full_next = 0;
static int sp_gc_collect_installed(void) {
  if (sp_gc_collect_fn) return 1;
  SP_GC_CTR_SET(sp_gc_threshold, (size_t)-1 / 4);
  return 0;
}
void sp_gc_collect_retune(void) {
  /* the retune hook inside sp_gc_collect adjusts the object threshold */
  if (!sp_gc_collect_installed()) return;
  sp_gc_collect_fn();
  sp_gc_enforce_mem_limit();
}
/* Used by sp_stw_collect to skip a redundant
   stop-the-world when another worker just collected. */
int sp_gc_collection_wanted(void) {
  /* Everything read atomically: this runs before the world is stopped
     (sp_stw_collect's early-out), concurrent with other workers' relaxed
     counter adds AND with the allocators' one-shot GC-stress threshold
     write (heap-locked, but this reader holds only the sched lock). The
     retune writes are plain but never overlap: they run while g_stw_active
     is set, and this is only called with it clear, under the same lock
     that publishes it. A stale read at worst skips one redundant
     collection. */
  return SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold);
}

/* sp_gc_alloc lives in lib/sp_slab.c: the allocation fast path is one
   function there, the slab's claim and the collector's bookkeeping in one
   frame. */
/* The safe points' flag (a loop's back-edge, a method's entry: the signals'
   sp_sig_pending_flag, read by lib/sp_cold.c's polls) and the collection
   asked of them. Defined here, which every runtime links, so asking needs
   nothing of sp_cold.o: a weak reference to a function there pulled
   sp_cold.o, and what it needs, into a C extension's link on macOS. */
volatile int sp_sig_pending_flag;
volatile int sp_gc_poll_due;
void *sp_gc_alloc_nogc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)sp_slab_alloc(need);
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0; h->old = 0; h->dirty = 0;
  if (fin) sp_slab_set_fin(h);
  if (sp_alloc_report_on) sp_alloc_report_count((void *)scn, sz);
  SP_HEAP_LOCK();
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
  SP_HEAP_UNLOCK();
#ifndef SP_GC_NONE
  /* A collection this allocation may not run (a Bignum operation's
     operands are temporaries nothing roots) is asked of the next safe point
     the program has, a loop's back-edge or a method's entry: a loop whose
     only allocations come this way (`s = (s * 75 + 74) % m`) otherwise never
     collected, and its memory grew with its round count (#11106). */
  if (SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold) && !sp_gc_poll_due) {
    sp_gc_poll_due = 1;
    SP_ATOMIC_STORE(&sp_sig_pending_flag, 1, __ATOMIC_RELEASE);
  }
#endif
  return (char *)h + sizeof(sp_gc_hdr);
}
/* The collection a safe point runs for sp_gc_alloc_nogc (sp_gc_poll_due):
   the one an allocation would have run, if it is still due */
void sp_gc_poll_collect(void) {
#ifndef SP_GC_NONE
#ifdef SP_THREADS
  if (SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold)) sp_stw_collect();
#else
  if (SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold)) sp_gc_collect_retune();
#endif
#endif
}

SP_TLS struct sp_str_lcache_entry sp_str_lcache[SP_STR_LCACHE_SIZE];

void sp_str_lcache_clear(void) {
  for (unsigned i = 0; i < SP_STR_LCACHE_SIZE; i++) sp_str_lcache[i].s = NULL;
}

/* PolyArray free-list pool (see sp_alloc.h). Bounded so a burst does not pin
   memory forever; an over-cap or oversized-buffer entry frees normally. The
   scan/finalize hooks stay valid on recycled headers -- only `next` and the
   heap-byte accounting change hands.
   PER THREAD. It was one Treiber stack for the process, pushed by every
   owner's sweep and popped by every allocating worker, and on a 32-core
   server the exchange on its head was the single hottest symbol (8.6% of
   the process, ahead of memmove). A worker sweeps its own young list, so
   what it recycles it can hand back to itself with no atomics at all; the
   sweeper threads and the barrier helpers recycle into their own, which
   the cap bounds. The cap is per thread for the same reason. */
SP_TLS sp_gc_hdr *sp_polyarr_pool_head = NULL;
SP_TLS long sp_polyarr_pool_count = 0;
/* The cap is per thread, so threaded it is a fraction of what the one
   process-wide pool held (up to 64k, which a tree benchmark churning tens
   of thousands of arrays a cycle leaned on): 8k a worker bounds thirty
   workers where 64k each would not, and single-threaded the one pool keeps
   the old cap. */
#ifdef SP_THREADS
#define SP_POLYARR_POOL_MAX 8192
#else
#define SP_POLYARR_POOL_MAX 65536
#endif
#define SP_POLYARR_POOL_KEEP_CAP 64   /* don't retain unusually large buffers */
void sp_PolyArray_pool_recycle(sp_gc_hdr *h) {
  sp_PolyArray *a = (sp_PolyArray *)((char *)h + sizeof(sp_gc_hdr));
  /* the slots a shift stepped past are the buffer's again (a reused array
     starts empty, so no element moves) */
  a->len = 0; SP_ARR_REBASE(a);
  if (sp_polyarr_pool_count >= SP_POLYARR_POOL_MAX || a->cap > SP_POLYARR_POOL_KEEP_CAP) {
    if (a->data != a->inl) sp_pl_free(a->data);
    sp_slab_free(h);
    return;
  }
  h->next = sp_polyarr_pool_head;
  sp_polyarr_pool_head = h;
  sp_polyarr_pool_count++;
}

/* Non-inline sp_str_alloc, for a TU that cannot include sp_alloc.h.
   lib/sp_bigint.c is the one: it pulls mruby_shim.h, whose sp_bool disagrees
   with sp_types.h's, so the header cannot be added alongside. Its Integer#to_s
   still allocates the same String object as other producers (#3396). */
char *sp_str_alloc_ext(size_t len) { return sp_str_alloc(len); }

/* Install the retune hook and read the floor before the first allocation. */
SP_CONSTRUCTOR static void sp_alloc_install_hooks(void) {
#ifndef SP_GC_NONE   /* (--gc=none: no collection runs them; #10699) */
  sp_gc_obj_retune_hook = sp_gc_retune_object;
#endif
  sp_alloc_floors_from_env();
}

/* Float#to_s / #inspect (declared in sp_alloc.h): shortest round-trip decimal.
   sp_float_shortest gives the shortest significant digits + decimal exponent
   with no locale dependency (pure integer arithmetic; see sp_dtoa.c); the
   fixed vs scientific layout is Ruby's Float#to_s rule (which differs from
   %g's), preserved from the previous strtod-probe implementation. */
const char *sp_float_to_s(sp_float f) {
  if(f!=f){char*r=sp_str_alloc_raw(4);r[0]='N';r[1]='a';r[2]='N';r[3]=0;return r;}
  if(f==HUGE_VAL||f==-HUGE_VAL){if(f<0){char*r=sp_str_alloc_raw(10);memcpy(r,"-Infinity",10);return r;}char*r=sp_str_alloc_raw(9);memcpy(r,"Infinity",9);return r;}
  if(f==0.0){if(signbit(f)){char*r=sp_str_alloc_raw(5);memcpy(r,"-0.0",5);return r;}char*r=sp_str_alloc_raw(4);memcpy(r,"0.0",4);return r;}
  int neg = signbit(f);
  char digits[32]; int dlen;
  int exp = sp_float_shortest(neg ? -f : f, digits, &dlen);
  int decpt = exp + 1;   /* number of digits before the decimal point in fixed form */
  char *out=sp_str_alloc_raw(64);int o=0;
  if(neg)out[o++]='-';
  /* fixed notation when the point sits within the digits (a fractional part,
     dlen>decpt) OR the integer part is <= 15 digits; a longer integer-valued
     value (dlen<=decpt, decpt>15) prints scientific like CRuby (#2593). */
  if(decpt>0&&(decpt<=15||dlen>decpt)){
    if(decpt<dlen){memcpy(out+o,digits,decpt);o+=decpt;out[o++]='.';memcpy(out+o,digits+decpt,dlen-decpt);o+=(dlen-decpt);}
    else{memcpy(out+o,digits,dlen);o+=dlen;for(int i=dlen;i<decpt;i++)out[o++]='0';out[o++]='.';out[o++]='0';}
  }
  else if(decpt<=0&&decpt>-4){
    out[o++]='0';out[o++]='.';for(int i=decpt;i<0;i++)out[o++]='0';memcpy(out+o,digits,dlen);o+=dlen;
  }
  else{
    out[o++]=digits[0];out[o++]='.';
    if(dlen==1)out[o++]='0';else{memcpy(out+o,digits+1,dlen-1);o+=(dlen-1);}
    out[o++]='e';int e=decpt-1;
    if(e>=0)out[o++]='+';else{out[o++]='-';e=-e;}
    if(e<10){out[o++]='0';out[o++]=(char)('0'+e);}
    else o+=snprintf(out+o,16,"%d",e);
  }
  out[o]=0;sp_str_set_len(out,(size_t)o);return out;
}

#ifndef SP_NO_ALLOC_REPORT
/* ---- SPINEL_ALLOC_REPORT: deterministic allocation counters (#1336) ----
   Env-var gated (set to 1 or an output path); zero work when off beyond one
   predictable branch at each allocation entry point. Counters key on the
   object's scan callback (the de-facto type identity); sp_alloc_report_tag
   attaches human names (builtins + user classes, registered by the generated
   prologue when the gate is on). Strings count separately (no scan fn).
   Dump: folded `alloc;<Type> <count>` lines plus `# bytes` comments, to the
   env value as a path, or stderr when it is "1". No signals, no allocation
   in the hot path, portable (plain counters + atexit). */
int sp_alloc_report_on = 0;
static int sp_alloc_sites_on = 0;
typedef struct { void *key; void *site; unsigned long long count, bytes; } sp_AllocStat;
/* Sized for the per-SITE case, which is what fills this table: one entry per
   (type, site) pair rather than one per type. Strings alone reach into the
   hundreds of sites on a Rails-scale app, and a full table silently merges
   into the home slot -- the one failure mode that would quietly misattribute
   the numbers this feature exists to report. BSS, so the untouched tail costs
   nothing when the report is off. */
#ifndef SP_ALLOC_STATS
#define SP_ALLOC_STATS 8192
#endif
static sp_AllocStat sp_alloc_stats[SP_ALLOC_STATS];
/* Type names live in their own table: one entry per scan fn, independent of
   how many sites allocate it. */
typedef struct { void *key; const char *name; } sp_AllocName;
#define SP_ALLOC_NAMES 512
static sp_AllocName sp_alloc_names[SP_ALLOC_NAMES];

static const char *sp_alloc_name_of(void *key) {
  size_t h = ((size_t)(uintptr_t)key >> 4) % SP_ALLOC_NAMES;
  for (size_t i = 0; i < SP_ALLOC_NAMES; i++) {
    sp_AllocName *n = &sp_alloc_names[(h + i) % SP_ALLOC_NAMES];
    if (n->key == key) return n->name;
    if (n->key == NULL) return NULL;
  }
  return NULL;
}
/* Allocations the table had no room to attribute. They used to be added to
   the probe's home slot -- a row belonging to a DIFFERENT (type, site) pair --
   which reads exactly like a real count, so a saturated run reported plausible
   and wrong numbers with nothing to say it had happened (#3481). Everything
   that lands here is instead kept out of the per-row numbers entirely and
   reported as its own line: the rows that remain are all true, and the part
   that was lost is visible. */
static sp_AllocStat sp_alloc_overflow;
static sp_AllocStat *sp_alloc_stat_slot(void *key, void *site) {
  size_t h = (((size_t)(uintptr_t)key >> 4) ^ ((size_t)(uintptr_t)site >> 3)) % SP_ALLOC_STATS;
  for (size_t i = 0; i < SP_ALLOC_STATS; i++) {
    sp_AllocStat *s = &sp_alloc_stats[(h + i) % SP_ALLOC_STATS];
    if ((s->key == key && s->site == site) || s->key == NULL) { s->key = key; s->site = site; return s; }
  }
  return &sp_alloc_overflow;
}
/* The frame that asked for this allocation: skip this helper, the counter and
   the allocator itself. */
static void *sp_alloc_site_now(void) {
#if SP_ALLOC_SITE_AVAILABLE
  if (!sp_alloc_sites_on) return NULL;
  /* frame 0 is this counter (sp_alloc_site_now inlines into it), frame 1 the
     allocator, frame 2 the code that asked -- which is what we want. */
  void *fr[4];
  int n = backtrace(fr, 4);
  return n >= 3 ? fr[2] : (n > 0 ? fr[n - 1] : NULL);
#else
  return NULL;
#endif
}
/* NULL is the table's empty marker, so a scan-less object (an int array, a
   plain byte buffer) counts under this stand-in key -- which keeps it on the
   per-site path too. */
#define SP_ALLOC_NOSCAN_KEY ((void *)(uintptr_t)1)
/* Strings carry no scan fn, so they get a reserved key of their own rather
   than a pair of standalone counters. Same table means the same per-site
   path: with SPINEL_ALLOC_SITES off every string lands in one slot (site
   NULL) and the dump is byte-identical to the old aggregate line, and with
   it on they split by caller like every other type. Strings are the largest
   share of allocated bytes in a typical app, so leaving them off the site
   path left the biggest question the report raises unanswerable. */
#define SP_ALLOC_STR_KEY ((void *)(uintptr_t)2)
/* Defined below, beside the dump it calls: a signal asked for the report and
   this is the first place after it that is allowed to write one. */
static void sp_alloc_report_poll(void);
void sp_alloc_report_count(void *scan, size_t bytes) {
  sp_alloc_report_poll();
  sp_AllocStat *s = sp_alloc_stat_slot(scan ? scan : SP_ALLOC_NOSCAN_KEY, sp_alloc_site_now());
  s->count++; s->bytes += (unsigned long long)bytes;
}
void sp_alloc_report_str(size_t bytes) {
  sp_alloc_report_poll();
  sp_AllocStat *s = sp_alloc_stat_slot(SP_ALLOC_STR_KEY, sp_alloc_site_now());
  s->count++; s->bytes += (unsigned long long)bytes;
}
void sp_alloc_report_tag(void *scan, const char *name) {
  size_t h = ((size_t)(uintptr_t)scan >> 4) % SP_ALLOC_NAMES;
  for (size_t i = 0; i < SP_ALLOC_NAMES; i++) {
    sp_AllocName *n = &sp_alloc_names[(h + i) % SP_ALLOC_NAMES];
    if (n->key == scan || n->key == NULL) { n->key = scan; n->name = name; return; }
  }
}
/* ---- naming a site ----
   backtrace_symbols reads only the DYNAMIC symbol table, and user methods are
   `static` C functions, so on its own it names nothing the user wrote: every
   site came out as `./app(+0x1bf67) [0x55..]`. The executable's full symbol
   table (.symtab) does carry the static functions whenever the binary was not
   stripped -- which --profile and -g builds guarantee and the default build
   leaves in place -- so on ELF the dump reads it from /proc/self/exe, once,
   and names the function containing the return address. When `--profile`
   also wrote `<exe>.symbols.json` beside the binary, that C name is turned
   into the Ruby name it was emitted for (`sp_Foo_bar` -> `Foo#bar`).

   All of this runs at dump time only and only for a site row: with
   SPINEL_ALLOC_SITES unset nothing here is ever reached. Every step degrades
   to the one before it -- no map: the C name; stripped or not ELF: the old
   backtrace_symbols text; nothing at all: the raw address. */
#if SP_ALLOC_SITE_AVAILABLE && defined(__linux__) && defined(__has_include)
#  if __has_include(<elf.h>)
#    include <elf.h>
#    include <ctype.h>
#    define SP_ALLOC_SYMTAB 1
#  endif
#endif
#ifndef SP_ALLOC_SYMTAB
#  define SP_ALLOC_SYMTAB 0
#endif

#if SP_ALLOC_SYMTAB
#if UINTPTR_MAX > 0xffffffffu
#  define SP_ELF(t) Elf64_##t
#else
#  define SP_ELF(t) Elf32_##t
#endif
typedef struct { uintptr_t lo, hi; const char *name; } sp_AllocSym;
typedef struct { char *c, *ruby; } sp_AllocRubyName;
#ifndef SP_THREADS
static int sp_alloc_syms_loaded = 0;
#endif
static sp_AllocSym *sp_alloc_syms = NULL;      /* FUNC symbols, sorted by lo */
static size_t sp_alloc_nsyms = 0;
static char *sp_alloc_symstr = NULL;           /* the .strtab the names point into */
static uintptr_t sp_alloc_load_bias = 0;
static sp_AllocRubyName *sp_alloc_rnames = NULL; /* from <exe>.symbols.json, sorted by c */
static size_t sp_alloc_nrnames = 0;

static int sp_alloc_sym_cmp(const void *a, const void *b) {
  uintptr_t x = ((const sp_AllocSym *)a)->lo, y = ((const sp_AllocSym *)b)->lo;
  return x < y ? -1 : x > y;
}
static int sp_alloc_rname_cmp(const void *a, const void *b) {
  return strcmp(((const sp_AllocRubyName *)a)->c, ((const sp_AllocRubyName *)b)->c);
}
static int sp_alloc_pread_all(int fd, void *buf, size_t n, off_t off) {
  char *p = (char *)buf;
  while (n > 0) {
    ssize_t r = pread(fd, p, n, off);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return 0;
    p += r; n -= (size_t)r; off += r;
  }
  return 1;
}
/* A JSON string body starting at `p` (just past the opening quote) into a
   fresh C string; *end is left past the closing quote. Only the escapes
   codegen's json_escape_into writes for a name matter here. */
static char *sp_alloc_json_str(const char *p, const char **end) {
  size_t cap = 32, n = 0;
  char *o = (char *)malloc(cap);
  if (!o) return NULL;
  while (*p && *p != '"') {
    char ch = *p++;
    if (ch == '\\' && *p) {
      char e = *p++;
      if (e == 'u') {               /* \u00XX: a control byte, kept as-is */
        unsigned v = 0; int k;
        for (k = 0; k < 4 && isxdigit((unsigned char)*p); k++, p++)
          v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
        ch = (char)v;
      }
      else ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
    }
    if (n + 2 > cap) { char *g = (char *)realloc(o, cap *= 2); if (!g) { free(o); return NULL; } o = g; }
    o[n++] = ch;
  }
  o[n] = 0;
  *end = *p == '"' ? p + 1 : p;
  return o;
}
/* `<exe>.symbols.json` is what --profile / --emit-symbol-map write: one
   {"c":"...","ruby":"...",...} object per emitted method. */
static void sp_alloc_load_ruby_names(const char *exe) {
  char path[4200];
  snprintf(path, sizeof path, "%s.symbols.json", exe);
  FILE *f = fopen(path, "r");
  if (!f) return;
  size_t cap = 1 << 16, len = 0;
  char *buf = (char *)malloc(cap);
  while (buf) {
    if (len + 4096 + 1 > cap) { char *g = (char *)realloc(buf, cap *= 2); if (!g) { free(buf); buf = NULL; break; } buf = g; }
    size_t r = fread(buf + len, 1, cap - len - 1, f);
    if (r == 0) break;
    len += r;
  }
  fclose(f);
  if (!buf) return;
  buf[len] = 0;
  size_t rcap = 0;
  for (const char *p = buf; (p = strstr(p, "{\"c\":\"")) != NULL; ) {
    const char *q;
    char *c = sp_alloc_json_str(p + 6, &q);
    if (!c) break;
    if (strncmp(q, ",\"ruby\":\"", 9) != 0) { free(c); p = q; continue; }
    char *rb = sp_alloc_json_str(q + 9, &q);
    p = q;
    if (!rb || !*c || !*rb) { free(c); free(rb); continue; }
    if (sp_alloc_nrnames == rcap) {
      size_t nc = rcap ? rcap * 2 : 256;
      sp_AllocRubyName *g = (sp_AllocRubyName *)realloc(sp_alloc_rnames, nc * sizeof *g);
      if (!g) { free(c); free(rb); break; }
      sp_alloc_rnames = g; rcap = nc;
    }
    sp_alloc_rnames[sp_alloc_nrnames].c = c;
    sp_alloc_rnames[sp_alloc_nrnames].ruby = rb;
    sp_alloc_nrnames++;
  }
  free(buf);
  if (sp_alloc_nrnames)
    qsort(sp_alloc_rnames, sp_alloc_nrnames, sizeof *sp_alloc_rnames, sp_alloc_rname_cmp);
}
/* Read the executable's .symtab once. Any surprise -- not ELF, another
   class or byte order, no .symtab (stripped) -- leaves the table empty and
   the caller falls back. */
static void sp_alloc_read_symtab(void) {
  char exe[4096];
  ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (el <= 0) return;
  exe[el] = 0;
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  SP_ELF(Ehdr) eh;
  SP_ELF(Shdr) *sh = NULL;
  SP_ELF(Sym) *st = NULL;
  int have_anchor = 0;
  if (!sp_alloc_pread_all(fd, &eh, sizeof eh, 0)) goto done;
  if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) goto done;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if (eh.e_ident[EI_DATA] != ELFDATA2LSB) goto done;
#else
  if (eh.e_ident[EI_DATA] != ELFDATA2MSB) goto done;
#endif
  if (eh.e_ident[EI_CLASS] != (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32)) goto done;
  if (eh.e_shentsize != sizeof(SP_ELF(Shdr)) || eh.e_shnum == 0 || eh.e_shnum > 65000) goto done;
  sh = (SP_ELF(Shdr) *)malloc((size_t)eh.e_shnum * sizeof *sh);
  if (!sh || !sp_alloc_pread_all(fd, sh, (size_t)eh.e_shnum * sizeof *sh, (off_t)eh.e_shoff)) goto done;
  for (size_t i = 0; i < eh.e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_entsize != sizeof(SP_ELF(Sym))) continue;
    if (sh[i].sh_link >= eh.e_shnum) break;
    SP_ELF(Shdr) *ss = &sh[sh[i].sh_link];
    size_t nsym = (size_t)(sh[i].sh_size / sizeof(SP_ELF(Sym)));
    st = (SP_ELF(Sym) *)malloc(nsym * sizeof *st);
    sp_alloc_symstr = (char *)malloc((size_t)ss->sh_size + 1);
    if (!st || !sp_alloc_symstr ||
        !sp_alloc_pread_all(fd, st, nsym * sizeof *st, (off_t)sh[i].sh_offset) ||
        !sp_alloc_pread_all(fd, sp_alloc_symstr, (size_t)ss->sh_size, (off_t)ss->sh_offset)) {
      free(sp_alloc_symstr); sp_alloc_symstr = NULL; break;
    }
    sp_alloc_symstr[ss->sh_size] = 0;
    sp_alloc_syms = (sp_AllocSym *)malloc(nsym * sizeof *sp_alloc_syms);
    if (!sp_alloc_syms) break;
    for (size_t k = 0; k < nsym; k++) {
      if (ELF32_ST_TYPE(st[k].st_info) != STT_FUNC || st[k].st_size == 0 || st[k].st_shndx == SHN_UNDEF) continue;
      if (st[k].st_name >= ss->sh_size) continue;
      sp_AllocSym *y = &sp_alloc_syms[sp_alloc_nsyms++];
      y->lo = (uintptr_t)st[k].st_value;
      y->hi = y->lo + (uintptr_t)st[k].st_size;
      y->name = sp_alloc_symstr + st[k].st_name;
      /* The anchor that gives the load bias: a PIE runs slid from the
         addresses the file records, by however much this function -- which
         is in the same executable as every site -- moved. 0 when fixed. */
      if (!have_anchor && strcmp(y->name, "sp_alloc_report_count") == 0) {
        sp_alloc_load_bias = (uintptr_t)&sp_alloc_report_count - y->lo;
        have_anchor = 1;
      }
    }
    qsort(sp_alloc_syms, sp_alloc_nsyms, sizeof *sp_alloc_syms, sp_alloc_sym_cmp);
    break;
  }
  /* Without the anchor the slide is unknown and every lookup would name the
     wrong function: better to name none. */
  if (sp_alloc_nsyms && !have_anchor) sp_alloc_nsyms = 0;
  if (sp_alloc_nsyms) sp_alloc_load_ruby_names(exe);
done:
  free(st);
  free(sh);
  close(fd);
}
/* A dump asked for by signal runs on the reader thread (or on whichever
   thread allocates next) and can overlap the one atexit writes, so under
   threads the tables are built under pthread_once: a second dump waits for
   the first to finish building them instead of reading them half-sorted. */
static void sp_alloc_load_symtab(void) {
#ifdef SP_THREADS
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, sp_alloc_read_symtab);
#else
  if (sp_alloc_syms_loaded) return;
  sp_alloc_syms_loaded = 1;
  sp_alloc_read_symtab();
#endif
}
/* The function whose body holds `site`, by its .symtab name; NULL when the
   address is not inside any (a shared library's frame, a stripped binary). */
static const char *sp_alloc_symtab_name(void *site) {
  sp_alloc_load_symtab();
  if (!sp_alloc_nsyms) return NULL;
  /* a return address is one past the call, which can be one past the end
     of a function that ends in a noreturn call: look up the byte before. */
  uintptr_t a = (uintptr_t)site - sp_alloc_load_bias - 1;
  size_t lo = 0, hi = sp_alloc_nsyms;
  while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (sp_alloc_syms[m].lo <= a) lo = m + 1; else hi = m; }
  /* the innermost of any overlapping entries is the last one starting at or
     below a; scan back a little for one that actually contains it */
  for (size_t k = lo, tries = 0; k > 0 && tries < 8; k--, tries++) {
    sp_AllocSym *y = &sp_alloc_syms[k - 1];
    if (a >= y->lo && a < y->hi) return y->name;
  }
  return NULL;
}
/* `sp_Foo_bar` (or a compiler clone of it, `sp_Foo_bar.constprop.0`) to the
   Ruby name the symbol map gives it; NULL without a map entry. */
static const char *sp_alloc_ruby_name(const char *cname) {
  if (!sp_alloc_nrnames) return NULL;
  char base[512];
  size_t n = strcspn(cname, ".");
  if (n >= sizeof base) return NULL;
  memcpy(base, cname, n); base[n] = 0;
  sp_AllocRubyName key = { base, NULL };
  sp_AllocRubyName *hit = (sp_AllocRubyName *)bsearch(&key, sp_alloc_rnames, sp_alloc_nrnames,
                                                      sizeof *sp_alloc_rnames, sp_alloc_rname_cmp);
  return hit ? hit->ruby : NULL;
}
#endif

/* A site's human name, resolved at dump time: the Ruby method name when the
   symbol map is beside the binary, else the C function from .symtab, else
   what the dynamic symbol table knows, else the raw address. Caller frees. */
static char *sp_alloc_site_name(void *site) {
#if SP_ALLOC_SYMTAB
  {
    const char *cn = sp_alloc_symtab_name(site);
    if (cn) {
      const char *rn = sp_alloc_ruby_name(cn);
      char *r = strdup(rn ? rn : cn);
      if (r) return r;
    }
  }
#endif
#if SP_ALLOC_SITE_AVAILABLE
  char **syms = backtrace_symbols(&site, 1);
  if (syms && syms[0]) {
    /* "path(sym+0x12) [0xaddr]" -> "sym" when the symbol is there */
    const char *o = strchr(syms[0], '(');
    const char *plus = o ? strchr(o, '+') : NULL;
    char *r;
    if (o && plus && plus > o + 1) {
      size_t n = (size_t)(plus - o - 1);
      r = (char *)malloc(n + 1);
      if (r) { memcpy(r, o + 1, n); r[n] = 0; free(syms); return r; }
    }
    r = strdup(syms[0]);
    free(syms);
    if (r) return r;
  }
  if (syms) free(syms);
#endif
  { char *r = (char *)malloc(32); if (r) snprintf(r, 32, "%p", site); return r; }
}
static void sp_alloc_report_dump(void) {
  const char *out = getenv("SPINEL_ALLOC_REPORT");
  FILE *f = stderr;
  int close_f = 0;
  if (out && out[0] && strcmp(out, "1") != 0) {
    FILE *g = fopen(out, "w");
    if (g) { f = g; close_f = 1; }
  }
  /* Symbolise the sites once, here: `alloc;<site>;<Type> <count>` keeps the
     folded-stack shape a flamegraph consumer wants, with the site as the outer
     frame. Without site tracking the line is the old per-type one. */
  for (int pass = 0; pass < 2; pass++) {
    const char *lead = pass ? "# bytes " : "alloc;";
    for (size_t i = 0; i < SP_ALLOC_STATS; i++) {
      sp_AllocStat *s = &sp_alloc_stats[i];
      if (!s->key || !s->count) continue;
      const char *nm = s->key == SP_ALLOC_NOSCAN_KEY ? "(no-scan)"
                     : s->key == SP_ALLOC_STR_KEY    ? "String"
                     : sp_alloc_name_of(s->key);
      char tybuf[64];
      if (!nm) { snprintf(tybuf, sizeof tybuf, "scan_%p", s->key); nm = tybuf; }
      unsigned long long v = pass ? s->bytes : s->count;
      if (s->site) {
        char *sym = sp_alloc_site_name(s->site);
        if (pass) fprintf(f, "# bytes %s;%s %llu\n", sym, nm, v);
        else      fprintf(f, "alloc;%s;%s %llu\n", sym, nm, v);
        free(sym);
      }
      else fprintf(f, "%s%s %llu\n", lead, nm, v);
    }
  }
  /* Say it out loud when the table saturated: the rows above are complete and
     correct as far as they go, and this is what they do not cover. Silence
     here is the failure this report must not have. */
  if (sp_alloc_overflow.count) {
    fprintf(f, "alloc;(unattributed) %llu\n", sp_alloc_overflow.count);
    fprintf(f, "# bytes (unattributed) %llu\n", sp_alloc_overflow.bytes);
    fprintf(f, "# note the stats table (%d entries) was full: %llu allocation(s)"
               " could not be attributed and are NOT counted in the rows above\n",
            (int)SP_ALLOC_STATS, sp_alloc_overflow.count);
  }
  if (close_f) fclose(f);
}
/* ---- a dump from a program that is still running ----
   The dump above runs from atexit, which a long-lived program never reaches:
   a server is stopped by a signal, so the counters it spent the whole run
   filling are lost at the moment they are worth reading. Nothing about the
   table needed changing -- it is complete at every instant -- it just had no
   way out before the process ended.

   The handler cannot write the report: fopen, malloc and backtrace_symbols
   are none of them async-signal-safe. What it can do is `write` one byte to a
   pipe, which is, and a thread parked on the other end does the writing. That
   thread is why the signal works on an IDLE server -- the first version polled
   the flag from the counting path, and a server with no traffic allocates
   nothing, so the dump waited for the next request instead of arriving when
   asked.

   Without threads there is no such thread to park, and a single-threaded
   program that is idle is inside a syscall with nothing else to run: there
   the flag and the counting path are the only mechanism available, and the
   dump lands on the next allocation. The handler picks whichever is armed. */
static volatile sig_atomic_t sp_alloc_report_pending = 0;
static int sp_alloc_report_pipe[2] = { -1, -1 };

static void sp_alloc_report_handler(int sig) {
  (void)sig;
  if (sp_alloc_report_pipe[1] >= 0) {
    char b = 1;
    /* Non-blocking, so a full pipe (or a fork'd child, which inherited the
       handler but not the thread that reads it) degrades to the flag rather
       than blocking inside a signal handler. */
    if (write(sp_alloc_report_pipe[1], &b, 1) == 1) return;
  }
  sp_alloc_report_pending = 1;
}

/* A number (so real-time signals work) or a name with or without the SIG
   prefix. SIGUSR1 by default because a program that wants it for itself can
   move this one, and the handler is installed only while the report is on.
   Kept as a small copy of sp_resolve_preempt_signal rather than a shared
   helper: the allocator sits below the scheduler and should not reach up into
   it for a dozen lines. */
static int sp_resolve_report_signal(void) {
  const char *e = getenv("SPINEL_ALLOC_REPORT_SIGNAL");
  if (!e || !*e) return SIGUSR1;
  char *end;
  long n = strtol(e, &end, 10);
  if (*end == '\0') { if (n > 0 && n < 65) return (int)n; }
  else {
    const char *name = e;
    if (strncasecmp(name, "SIG", 3) == 0) name += 3;
    static const struct { const char *n; int s; } tab[] = {
      { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "URG", SIGURG },
      { "IO", SIGIO }, { "WINCH", SIGWINCH },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
      if (strcasecmp(name, tab[i].n) == 0) return tab[i].s;
  }
  fprintf(stderr, "spinel: ignoring unrecognized SPINEL_ALLOC_REPORT_SIGNAL=%s;"
                  " using SIGUSR1\n", e);
  return SIGUSR1;
}

/* The flag path: the next counted allocation writes the report. Claimed with
   an exchange so that however many threads are allocating, one dump happens
   and the losers carry straight on. */
static void sp_alloc_report_poll(void) {
  if (!sp_alloc_report_pending) return;
#ifdef SP_THREADS
  if (!SP_ATOMIC_EXCHANGE(&sp_alloc_report_pending, 0, __ATOMIC_SEQ_CST)) return;
#else
  sp_alloc_report_pending = 0;
#endif
  sp_alloc_report_dump();
}

#ifdef SP_THREADS
/* Parked on the pipe for the life of the process. Every dump rewrites the
   whole cumulative table, the same one atexit writes, so a window is two
   dumps subtracted rather than a mode of its own -- which is also what
   excludes a server's boot from the profile. Two signals in quick succession
   write it twice; the second overwrites the first, which is harmless. */
static void *sp_alloc_report_reader(void *arg) {
  (void)arg;
  for (;;) {
    char b;
    ssize_t n = read(sp_alloc_report_pipe[0], &b, 1);
    if (n == 0) return NULL;
    if (n < 0) { if (errno == EINTR) continue; return NULL; }
    sp_alloc_report_dump();
  }
}

/* The read end stays blocking, which is how the thread parks for the life of
   the process at no cost; only the WRITE end is non-blocking, because that is
   the one a signal handler touches and it must never block. Both are
   close-on-exec, so an exec'd child does not inherit a pipe nobody reads.
   Failure here is not fatal: the handler falls back to the flag. */
static void sp_alloc_report_start_reader(void) {
  if (pipe(sp_alloc_report_pipe) != 0) { sp_alloc_report_pipe[0] = sp_alloc_report_pipe[1] = -1; return; }
  fcntl(sp_alloc_report_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(sp_alloc_report_pipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(sp_alloc_report_pipe[1], F_SETFL,
        fcntl(sp_alloc_report_pipe[1], F_GETFL, 0) | O_NONBLOCK);
  pthread_t tid;
  if (pthread_create(&tid, NULL, sp_alloc_report_reader, NULL) != 0) {
    close(sp_alloc_report_pipe[0]); close(sp_alloc_report_pipe[1]);
    sp_alloc_report_pipe[0] = sp_alloc_report_pipe[1] = -1;
    return;
  }
  pthread_detach(tid);
}
#endif

SP_CONSTRUCTOR static void sp_alloc_report_boot(void) {
  const char *e = getenv("SPINEL_ALLOC_REPORT");
  if (e && *e && strcmp(e, "0") != 0) {
    sp_alloc_report_on = 1; sp_gc_alloc_fast_ok = 0;
    { const char *sv = getenv("SPINEL_ALLOC_SITES");
      sp_alloc_sites_on = (sv && *sv && strcmp(sv, "0") != 0) ? 1 : 0; }
#ifdef SP_THREADS
    sp_alloc_report_start_reader();
#endif
    /* SA_RESTART: asking for a report must not turn an in-flight read(2) into
       an EINTR the program never expected to handle. */
    { struct sigaction sa; memset(&sa, 0, sizeof sa);
      sa.sa_handler = sp_alloc_report_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      sigaction(sp_resolve_report_signal(), &sa, NULL); }
    atexit(sp_alloc_report_dump);
  }
}
#endif /* !SP_NO_ALLOC_REPORT */
