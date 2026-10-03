/* sp_alloc.c -- the single, shared definitions backing sp_alloc.h.

   Owns the string heap so that both the generated program and every standalone
   lib/*.c allocate onto one heap. sp_str_sweep is registered with the object GC
   via a constructor, so a collection triggered from any TU also reaps strings. */
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

#ifdef SP_THREADS
sp_str_wslot_t sp_str_wslot[SP_MAX_WORKERS];     /* zero-init: NULL lists, 0 bytes */
/* Aggregate live string bytes across every worker's list. Called only off the
   fast path (collection trigger uses the per-worker slice; sweep/retune here). */
size_t sp_str_bytes_total(void) {
  size_t s = 0;
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  for (int i = 0; i < n; i++) s += SP_GC_CTR_GET(sp_str_wslot[i].young_bytes);
  return s;
}
#else
sp_str_hdr *sp_str_heap = NULL;
size_t sp_str_heap_bytes = 0;
sp_str_hdr *sp_str_old = NULL;
size_t sp_str_old_bytes = 0;
#endif
/* SPINEL_GC_OBJ_BUDGET: how much of the mark set the object collection budget
   is priced from. 0 = the object heap alone (`obj`), 1 = the whole set a mark
   walks (`walk`), 2 = gated on what the last collection cost (the default).
   Read once, beside the other boot-time GC modes. */
int sp_gc_obj_budget_mode = 2;
/* The last gate decision, in 1024ths, so the stats line can report it and a
   test can read it. 1024 is `walk`, 0 is `obj`. */
size_t sp_gc_obj_alpha1024 = 1024;
/* SPINEL_GC_OBJ_BUDGET=fixed / SPINEL_GC_STR_BUDGET=fixed: hold that heap's
   budget at its floor instead of re-aiming it after every collection. Read
   once beside the other boot-time GC modes; see the comment there. */
int sp_gc_obj_budget_fixed = 0;
int sp_gc_str_budget_fixed = 0;
/* SPINEL_GC_STR_MAJOR=fixed: hold the string old generation's gate at its floor
   instead of re-aiming it, adapting nothing. */
int sp_gc_str_major_fixed = 0;
/* The major runs on a SCHEDULE, with the size test demoted to a backstop, the
   way the object heap has always run its full collection. On by default;
   SPINEL_GC_STR_MAJOR=size is the way back to the gate that shipped before it.
   See the block above sp_str_major_interval for what it is measured to cost
   and save. */
int sp_gc_str_major_sched = 1;
/* String majors run on their own gate, so the object collector's `full` count
   does not describe them: pinning that gate changed the old generation from
   57.2 MB to 11.5 MB with the reported full count identical at 6. */
size_t sp_gc_str_majors = 0;
size_t sp_str_old_threshold = 1024 * 1024;
size_t sp_str_old_threshold_init = 1024 * 1024;
/* How many string sweeps between majors, and the count that drives it. The cadence is a COUNT rather than a size because a
   size gate re-aimed from the old list is aimed at a number the same gate
   produced: a small budget promotes early, promotion is one-way until a major,
   and "twice what the last major left" then sets the next gate from what early
   promotion inflated (#4407). The object heap has never gated its full
   collection that way -- sp_gc.c runs it on an interval and keeps the size test
   as a backstop for growth between scheduled fulls -- and this is that policy,
   on the heap it was missing from. The bounds are the object heap's, for the
   same reason its comment gives.

   Measured on four shapes (#4407). On a real application at a 16 MB string
   floor -- ONCE Campfire, measured by the reporter -- PSS went 280 to 145 MB
   and throughput 345 to 365 req/s: the schedule went PAST the 64 MB control on
   memory and was faster. On a 12 MB live set under a 4 MB floor it cut the old
   generation from 67.4 MB to 21.4 MB and peak RSS from 135 MB to 84 MB, and ran
   no slower. On the adversarial shape -- 400,000 retained strings that no major
   can free, where a cadence firing too often would be pure cost -- there is no
   difference at all in either wall time or RSS, because the survival ratio
   stretches the interval to 16 and then to 128. The one cost is
   benchmark/bm_threaded_render.rb, where twelve order-flipped passes a side put
   median RSS at 776 MB against 736, with wall time identical (1.93s against
   1.92s). Five percent of one benchmark's median memory, no time, against
   halving a real application's. */
#define SP_STR_MAJOR_INTERVAL 8
#define SP_STR_MAJOR_INTERVAL_MAX 128
static int sp_str_major_interval = SP_STR_MAJOR_INTERVAL;
static unsigned sp_str_sweep_cycle = 0;
static int sp_str_major_forced = 0;
/* The old generation's size at every string sweep, so the run can be described
   by its shape rather than by whichever instant a per-second line happened to
   catch. rubys' reading on #4407 is that the ratio of the MEDIAN to the MINIMUM
   says whether the gate is holding garbage: the minimum is about what a major
   can actually leave, so the ratio is how far above that the gate keeps the
   heap. It could not be checked on a two-second benchmark, because the [gcph]
   line prints once a second and a median of three samples is not a median.
   Sampling at the sweep removes that limit: the cadence becomes the
   collector's, not the clock's. A ring, so a long run costs no more than a
   short one and the samples are the most recent SP_STR_SHAPE_MAX. */
#define SP_STR_SHAPE_MAX 8192
static size_t sp_str_shape[SP_STR_SHAPE_MAX];
static unsigned sp_str_shape_n = 0;      /* total sweeps seen */
static int sp_str_shape_cmp(const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}
/* The [gcph] line names whichever policy is running, so the number after it is
   never read as the other one's: the default's is a size to cross, the
   schedule's is a cadence with the size demoted to a backstop. */
static const char *sp_str_major_label(void) {
  static char buf[64];
  if (!sp_gc_str_major_sched) return "at ";
  snprintf(buf, sizeof buf, "every %d sweeps, backstop ", sp_str_major_interval);
  return buf;
}

/* The slab strings' old generation: not a list anybody walks but bits in
   the chunks (lib/sp_slab.c), so its bytes are what the MARK counted --
   everything it reached at a major, plus what it promoted at each minor --
   settled at sp_str_sweep_end. The lists below hold only the strings too
   large for the slab. */
static size_t sp_str_old_slab_bytes = 0;
extern size_t sp_gc_mk_str_bytes, sp_gc_mk_str_young_bytes;   /* lib/sp_gc.c: this cycle's mark */
/* After every mark (lib/sp_gc.c): a full cycle's mark reached every live
   slab string, and the sweep of the bitmaps leaves exactly those; a minor's
   promoted what it reached of the young. Every cycle, whatever the string
   gate decided for the lists: the chunk sweep frees young slab strings on
   every cycle and old ones on every full one. */
void sp_str_mark_settle(int full) {
  if (full) sp_str_old_slab_bytes = sp_gc_mk_str_bytes;
  else sp_str_old_slab_bytes += sp_gc_mk_str_young_bytes;
}
/* Live bytes in the old generation: the slab's, and every worker's list. */
static size_t sp_str_old_total(void) {
#ifdef SP_THREADS
  size_t t = sp_str_old_slab_bytes;
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  for (int i = 0; i < n; i++) t += SP_GC_CTR_GET(sp_str_wslot[i].old_bytes);
  return t;
#else
  return sp_str_old_slab_bytes + SP_GC_CTR_GET(sp_str_old_bytes);
#endif
}

/* Every live string byte, young and old, in either build. Both retunes need it
   and the young half is spelled differently with and without threads. */
static size_t sp_str_live_total(void) {
#ifdef SP_THREADS
  return sp_str_bytes_total() + sp_str_old_total();
#else
  return sp_str_heap_bytes + sp_str_old_total();
#endif
}
size_t sp_str_threshold = 256 * 1024;
size_t sp_str_threshold_init = 256 * 1024;
int sp_str_stress_checked = 0;

const char sp_str_empty_data[] = "\xff";
/* nil.to_s, true.to_s and false.to_s answer FROZEN Strings in CRuby: a
   header-carrying static object marked 0xf1, as a frozen literal is */
static struct { sp_str_hdr h; unsigned char m; char d[1]; } sp_fz_empty_s = { { NULL, 1 | SP_STR_SIZE_ASCII7, 0, 0 }, 0xf1, "" };
static struct { sp_str_hdr h; unsigned char m; char d[5]; } sp_fz_true_s = { { NULL, 5 | SP_STR_SIZE_ASCII7, 4, 0 }, 0xf1, "true" };
static struct { sp_str_hdr h; unsigned char m; char d[6]; } sp_fz_false_s = { { NULL, 6 | SP_STR_SIZE_ASCII7, 5, 0 }, 0xf1, "false" };
const char *const sp_str_frozen_empty = sp_fz_empty_s.d;
const char *const sp_str_frozen_true = sp_fz_true_s.d;
const char *const sp_str_frozen_false = sp_fz_false_s.d;
/* the link a chilled String's header carries (sp_str_is_chilled) */
const sp_str_hdr sp_str_chilled_tag = { NULL, 0, 0, 0 };

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
  if (!sp_str_stress_checked) {
    sp_str_stress_checked = 1;
    if (stress) { sp_str_threshold = 2048; sp_str_threshold_init = 2048; sp_gc_stress_pin = 1; }
  }
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
   while burning 2.9x the CPU, all of it in park/mark/unpark. The string heap
   already avoids this by comparing per-worker bytes, which makes its aggregate
   bound N * threshold; this gives the object heap the same bound.

   The cost is bounded and small: N * 256 KB of garbage retained between
   collections, 2 MB at eight workers. SPINEL_GC_THRESHOLD_KB overrides the
   base for a program that wants to trade more memory for fewer stops.

   Not scaled under GC stress: that mode exists to maximize collections, and
   multiplying its 2 KB budget would quietly weaken every stress run. */
#endif  /* SP_THREADS -- the floors below are read by EVERY program */
/* Set one heap's floor from an environment variable, leaving the other alone.
   SPINEL_GC_THRESHOLD_KB moves both together, and moving them together cannot
   answer WHICH heap's trigger paces the collections. On a server whose string
   live set grows 26x across a concurrency ladder while its object live set
   grows 4.9x, the mark walks both and only one of them decides when to look:
   raising just the object floor says whether that is the pacer, and raising
   just the string floor is the same question from the other side (#4384).
   The per-heap variable wins when both are set, being the more specific. */
static void sp_alloc_floor_from_env(const char *name, size_t *cur, size_t *init) {
  const char *e = getenv(name);
  if (!e || !*e) return;
  long v = atol(e);
  if (v <= 0) return;
  size_t base = (size_t)v * 1024;
  SP_GC_CTR_SET(*cur, base);
  *init = base;
}
/* The three floors, read from the environment. Called twice on purpose and
   idempotent: once before main for EVERY program, and again from
   sp_alloc_worker_tune, which only a threaded one reaches and which scales
   what it finds by the worker count.

   It used to live only in the second, and so did nothing at all in a
   single-threaded program -- `SPINEL_GC_THRESHOLD_KB=65536 ./prog` collected
   at 256 KB and said `trigger 0.25 MB` while the operator read the manual.
   The budget MODE was moved out of here for the same reason and with the same
   sentence (see sp_gc.c): the pacing is not a threads-only question. */
void sp_alloc_floors_from_env(void) {
  const char *e = getenv("SPINEL_GC_THRESHOLD_KB");
  if (e && *e) {
    long v = atol(e);
    if (v > 0) {
      size_t base = (size_t)v * 1024;
      SP_GC_CTR_SET(sp_gc_threshold, base); sp_gc_threshold_init = base;
      SP_GC_CTR_SET(sp_str_threshold, base); sp_str_threshold_init = base;
    }
  }
  sp_alloc_floor_from_env("SPINEL_GC_THRESHOLD_OBJ_KB", &sp_gc_threshold, &sp_gc_threshold_init);
  sp_alloc_floor_from_env("SPINEL_GC_THRESHOLD_STR_KB", &sp_str_threshold, &sp_str_threshold_init);
  /* The string heap's OLD generation has its own gate, and it is the one
     nothing could reach. A string is promoted the first sweep it survives, and
     an old string is reclaimed only by a MAJOR. SPINEL_GC_FULL_INTERVAL does
     not touch it -- that gates the OBJECT full cycle, which is why forcing
     every collection full changed nothing on a program whose memory was all in
     the string old list (#4407). This sets the floor under the growth backstop,
     and is the control that lets the cadence be measured against a pinned one. */
  sp_alloc_floor_from_env("SPINEL_GC_STR_MAJOR_KB", &sp_str_old_threshold, &sp_str_old_threshold_init);
  /* SPINEL_GC_STRESS=2 last, over any floor above: the two thresholds are
     pinned at zero, so every allocation that can collect does. Here because
     both callers must end on it -- a floor read after it put the trigger
     back, and the level then collected as seldom as the floor said -- and
     marked as checked, so the allocators' own one-shot (2048) does not run
     after it. */
  { const char *st = getenv("SPINEL_GC_STRESS");
    if (st && atoi(st) >= 2) {
      SP_GC_CTR_SET(sp_gc_threshold, 0); sp_gc_threshold_init = 0;
      SP_GC_CTR_SET(sp_str_threshold, 0); sp_str_threshold_init = 0;
      sp_gc_stress_pin = 1; sp_gc_stress_checked = 1; sp_str_stress_checked = 1;
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
     multiplier is the POOL size (min(cores, SPINEL_WORKERS)), not the thread
     count the program asked for, so the damage scales with the machine
     (#4146). */
  { size_t cur = SP_GC_CTR_GET(sp_gc_threshold);
    if (sp_gc_threshold_init > cur) SP_GC_CTR_SET(sp_gc_threshold, sp_gc_threshold_init); }
}
#endif

/* Re-tune the object / string GC thresholds from the pre-collect live bytes
   (the heuristic mirrors the original inline code in sp_gc_alloc / sp_str_alloc). */
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
   because that runs at the end of every collection and this file is where both
   thresholds and the string heap are visible. */
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
static void sp_gc_stats_emit(void) {
  static double first = 0;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  if (first == 0) first = now;
  double wall = now - first;
  unsigned long long n = sp_gc_stat_collections;
#ifdef SP_THREADS
  int nw = sp_active_workers; if (nw < 1) nw = 1;
  /* Both generations: survivors are promoted, so the young total alone reads
     as ~0 right after a collection and would say the string heap is empty. */
  size_t sbytes = sp_str_bytes_total() + sp_str_old_total();
#else
  int nw = 1;
  size_t sbytes = SP_GC_CTR_GET(sp_str_heap_bytes) + SP_GC_CTR_GET(sp_str_old_bytes);
#endif
  fprintf(stderr,
          "[gc] %llu collections (%llu full) in %.2fs of %.1fs wall (%.1f%%), %.2fms avg; "
          "live %.1f MB obj + %.1f MB str; trigger %.1f MB obj + %.2f MB str/worker x %d; "
          "mark share %.2f\n",
          n, sp_gc_stat_fulls, sp_gc_stat_seconds, wall,
          wall > 0 ? 100.0 * sp_gc_stat_seconds / wall : 0.0,
          n ? 1000.0 * sp_gc_stat_seconds / (double)n : 0.0,
          (double)SP_GC_CTR_GET(sp_gc_bytes) / 1048576.0, (double)sbytes / 1048576.0,
          (double)SP_GC_CTR_GET(sp_gc_threshold) / 1048576.0,
          (double)SP_GC_CTR_GET(sp_str_threshold) / 1048576.0, nw,
          (double)sp_gc_obj_alpha1024 / 1024.0);
  if (!sp_gc_ph_on) return;
  /* Which part of a collection cost that time. The names are the ones the
     collector's own comments use, so a number leads to the code that spent it.
     Under SP_THREADS the per-worker string sweep runs inside the slot sweep
     (sp_sweep_one_slot), so `string sweep` is the serial path's figure and
     reads zero on the threaded one. */
  /* Which generation the string live set is in. The [gc] line's `str` is the
     two added together, and they answer different questions: young is what the
     next sweep can reclaim, old is what only a MAJOR can, and a budget that
     promotes early can grow the second while the first looks healthy. */
  fprintf(stderr,
          "[gcph] string live %.1f MB young + %.1f MB old  "
          "(major %s%.1f MB old, %llu so far)\n",
          (double)(sp_str_live_total() - sp_str_old_total()) / 1048576.0,
          (double)sp_str_old_total() / 1048576.0,
          sp_str_major_label(),
          (double)sp_str_old_threshold / 1048576.0,
          (unsigned long long)sp_gc_str_majors);
  /* median / min over the sweeps, and their ratio. One number for the sawtooth
     the per-second line can only show a slice of. */
  if (sp_str_shape_n > 0) {
    unsigned n = sp_str_shape_n < SP_STR_SHAPE_MAX ? sp_str_shape_n : SP_STR_SHAPE_MAX;
    size_t *cp = (size_t *)malloc((size_t)n * sizeof *cp);
    if (cp) {
      memcpy(cp, sp_str_shape, (size_t)n * sizeof *cp);
      qsort(cp, n, sizeof *cp, sp_str_shape_cmp);
      double med = (double)cp[n / 2], mn = (double)cp[0];
      fprintf(stderr,
              "[gcph] string old over %u sweeps: min %.1f MB  median %.1f MB  max %.1f MB"
              "  (median/min %.1fx)\n",
              n, mn / 1048576.0, med / 1048576.0, (double)cp[n - 1] / 1048576.0,
              mn > 0 ? med / mn : 0.0);
      free(cp);
    }
  }
  fprintf(stderr,
          "[gcph] marked %llu objs  swept %llu slots\n",
          (unsigned long long)SP_GC_CTR_GET(sp_gc_ct_marked), (unsigned long long)SP_GC_CTR_GET(sp_gc_ct_swept));
  fprintf(stderr,
          "[gcph] mark %.3fs  old sweep %.3fs  slot sweep %.3fs (longest task %.3fs; tasks total %.3fs = obj %.3fs + str old %.3fs + str young %.3fs)  "
          "remembered clear %.3fs  string sweep %.3fs  trim %.3fs  of %.3fs total\n",
          sp_gc_ph_mark, sp_gc_ph_oldsweep, sp_gc_ph_slotsweep, sp_gc_ph_slot_max,
          sp_gc_ph_task_sum, sp_gc_ph_task_obj, sp_gc_ph_task_sold, sp_gc_ph_task_syoung,
          sp_gc_ph_rembclear, sp_gc_ph_strsweep, sp_gc_ph_trim,
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
      fprintf(stderr, "[gcph] concurrent sweep, applied under the barrier: objects %.3fs  strings %.3fs  slab release %.3fs  (the join at the top of the collection: %.3fs)\n",
              sp_gc_ph_apply_obj, sp_gc_ph_apply_str, sp_gc_ph_apply_release, sp_gc_ph_wait_top); }
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
  /* The budget is what may be ALLOCATED before the next collection, and what
     pays for it is what that collection COSTS. A collection marks BOTH heaps,
     so an object budget taken from the object live set alone is priced off
     the wrong quantity: rubys measured a ladder where the string live set
     grows 26x while the object set grows 4.9x, the collection rate falls with
     the object set, and the mark per request rises 2.5x (#4384).
     This is the default. SPINEL_GC_OBJ_BUDGET=obj restores pricing it off
     the object heap alone.

     It shipped opt-in first, because the argument was sound and the evidence
     was not: what had been measured at +47% was a fixed 16 MB FLOOR, which is
     a different policy -- a floor stops the budget getting small, this makes
     it proportional. rubys then ran both, on two emits at two concurrencies,
     twice each. Pricing it off the walk reproduces the floor's throughput
     (+26% to +44%) at within 5-10% of the floor's memory, and the reason to
     prefer it is neither of those: it settles at 70-78 MB where the floor
     pins 128, and at 227-253 MB where the floor is too small, so it is right
     at both ends of a 3.3x concurrency swing. A fixed number cannot be.
     Our own 61 benchmarks and optcarrot are neutral on it: same wall, RSS
     within 0.5%, fps inside its spread.
     What it cost, before the gate below: it widened the budget by the mark
     set whether or not the mark was what the program paid for. Two synthetics
     here that hold a large live string set while collecting cheaply paid
     memory for nothing. */

  /* ---- the gate: widen by the share of the collection the MARK is ----

     alpha is that share, and the budget widens by alpha x the string live
     set. A program whose collections are nearly all mark gets `walk`; one
     whose collections are nearly all sweep gets `obj`; the two synthetics and
     rubys' server sit at opposite ends of it rather than needing different
     defaults.

     COUNTS, not bytes. A cost ratio taken per byte does not carry between
     programs: a cache of large strings and a churn of small arrays hold the
     same megabytes with slot counts fifty times apart, which is how the first
     attempt at this failed (#4384). The sweep's cost is per SLOT and the
     mark's is per LIVE OBJECT, so counted, the coefficients are properties of
     this code rather than of a program's allocation sizes.

     And the coefficient is an ORDER, not a measurement. Measured here, mark
     is ~360-560 ns an object and sweep is ~8 ns a slot serially against ~120
     ns across eight workers, where the parked-worker coordination and the
     string sweep fold in. Writing those numbers down would pin this machine's
     ratio into the collector and be wrong on the next one. Written as the
     order they sit at -- Cm/Cs is about 64 serially and about 4 in parallel --
     the arithmetic is a shift and the answer barely moves: on the pair that
     motivated the gate, the measured coefficients give alpha 0.012 and 0.57,
     the orders give 0.016 and 0.55. The decision was never close enough for
     the precision to matter, which is the argument for not claiming it. */
  static size_t prev_marked = 0, prev_swept = 0;
  size_t cmk = SP_GC_CTR_GET(sp_gc_ct_marked), csw = SP_GC_CTR_GET(sp_gc_ct_swept);
  size_t marked = cmk > prev_marked ? cmk - prev_marked : 0;
  size_t swept  = csw > prev_swept  ? csw - prev_swept  : 0;
  prev_marked = cmk; prev_swept = csw;
  size_t alpha = 1024;   /* nothing measured yet: widen, which is what `walk` did */
  {
    int nw = 1;
#ifdef SP_THREADS
    nw = sp_active_workers; if (nw < 1) nw = 1;
#endif
    /* The parallel sweep pays for parking and waking the workers that help it,
       and folds the string sweep in, so a slot costs an order more there. */
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
  size_t walk = live;
  { size_t str = sp_str_live_total();
    walk += (str / 1024) * alpha + ((str % 1024) * alpha) / 1024; }
  /* saturating: the live counter is a heuristic and is allowed to lag, so it
     can read above the pre-collect total. Wrapping made `freed` enormous, the
     productive-sweep test went false, and the threshold was taken from a live
     count that had itself wrapped. */
  size_t freed = before > live ? before - live : 0;
  /* The productivity test stays on the OBJECT numbers -- this sweep is what
     frees object bytes, and whether it was worth running is a question about
     those. Only the budget it sets can be sized from the whole walk. */
  if (freed < before / 4) { sp_gc_threshold = sp_gc_sat_mul(before + (walk - live), 2); }
  else if (live > 0) { sp_gc_threshold = sp_gc_sat_mul(walk, 2); if (sp_gc_threshold < sp_gc_threshold_init) sp_gc_threshold = sp_gc_threshold_init; }
  else { sp_gc_threshold = sp_gc_threshold_init; }
}
/* `before` and `after` are both the WHOLE live string set -- young plus old --
   the way sp_gc_retune_object reads the whole object heap. Sizing from the
   young generation alone left this budget blind to the old one: a render
   promotes what it keeps, so `after` read as ~0 however much string data the
   process was holding, and the trigger fell back to its floor after every
   sweep. What it gates is a whole-heap stop-the-world, old generation
   included, so the budget that pays for a collection has to see the bytes the
   mark walks. Both sides have to move together -- a whole-heap `after` against
   a young-only `before` reads as an unproductive sweep every time.

   The threshold is the PER-WORKER budget (each worker triggers on its own
   list, so the aggregate heap is bounded by N * threshold). Retune on the
   per-worker average so the budget tracks a single worker's share and does NOT
   inflate by N each cycle -- retuning on the aggregate would grow it
   geometrically for long-lived strings. The single-threaded build works in
   absolute bytes (N == 1). */
static size_t sp_str_gate_old = 0;   /* the old total at the gate, for `before` */
/* Young bytes to leave out of the retune's "after": the concurrent sweep
   retunes at the next barrier, by which time the young lists hold a cycle of
   new allocation that the swept generation never contained. */
static size_t sp_str_retune_young_exclude = 0;
static void sp_str_retune(size_t before, size_t promoted) {
  if (sp_gc_stress_pin || sp_gc_str_budget_fixed) { sp_str_threshold = sp_str_threshold_init; return; }
#ifdef SP_THREADS
  int nw = sp_active_workers; if (nw < 1) nw = 1;
  size_t yb = sp_str_bytes_total();
  yb = yb > sp_str_retune_young_exclude ? yb - sp_str_retune_young_exclude : 0;
  size_t after = (yb + sp_str_old_total()) / (size_t)nw;
  before = (before + sp_str_gate_old) / (size_t)nw;
  (void)promoted;   /* already inside old_total by the time we run */
#else
  /* sp_str_old_total() already carries what this sweep promoted, so a promoted
     string is counted once, as the survivor it is: leaving it out would read as
     a very productive sweep and shrink the trigger, collecting harder and
     harder as the old generation grows. */
  size_t after = sp_str_heap_bytes + sp_str_old_total();
  before += sp_str_gate_old;
#endif
  size_t freed = before > after ? before - after : 0;   /* saturating; see sp_gc_retune_object */
  if (freed < before / 4) { sp_str_threshold = sp_gc_sat_mul(before, 2); }
  else if (after > 0) { sp_str_threshold = sp_gc_sat_mul(after, 2); if (sp_str_threshold < sp_str_threshold_init) sp_str_threshold = sp_str_threshold_init; }
  else { sp_str_threshold = sp_str_threshold_init; }
}

/* Collect and re-tune. The caller guarantees exclusive heap access: the
   single-threaded allocators hold sp_heap_lock; the threaded build runs the
   _all variant under stop-the-world (every other worker parked), via
   sp_stw_collect, so neither heap is mutated during the sweep. The object and
   string variants retune only their own threshold, matching the original
   per-heap inline collection so the single-threaded path stays byte-identical;
   _all retunes both since one stop-the-world sweeps both heaps. */
void sp_gc_collect_retune(void) {
  /* the retune hook inside sp_gc_collect adjusts the object threshold */
  sp_gc_collect();
  sp_gc_enforce_mem_limit();
}
void sp_str_collect_retune(void) {
  /* the gated sweep hook inside sp_gc_collect retunes the string threshold */
  sp_gc_collect();
}
void sp_gc_collect_retune_all(void) {
  sp_gc_collect();
  sp_gc_enforce_mem_limit();
}
/* Either heap over its trigger? Used by sp_stw_collect to skip a redundant
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
#ifdef SP_THREADS
  /* The string trigger is PER WORKER (sp_str_alloc compares this worker's own
     bytes), so the justified-now condition on the aggregate is N * threshold.
     Comparing the aggregate against the bare threshold made this true almost
     immediately at N > 1, so the early-out never suppressed a redundant stop
     and workers queued up behind each other's collections. */
  { int nw = sp_active_workers; if (nw < 1) nw = 1; if (nw > SP_MAX_WORKERS) nw = SP_MAX_WORKERS;
    return SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold) ||
           sp_str_bytes_total() > SP_GC_CTR_GET(sp_str_threshold) * (size_t)nw; }
#else
  return SP_GC_CTR_GET(sp_gc_bytes) > SP_GC_CTR_GET(sp_gc_threshold) ||
         SP_GC_CTR_GET(sp_str_heap_bytes) > SP_GC_CTR_GET(sp_str_threshold);
#endif
}

/* sp_gc_alloc lives in lib/sp_slab.c: the allocation fast path is one
   function there, the slab's claim and the collector's bookkeeping in one
   frame. */
void *sp_gc_alloc_nogc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)sp_slab_alloc(need);
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0; h->old = 0; h->dirty = 0;
  if (fin) sp_slab_set_fin(h);
  if (sp_alloc_report_on) sp_alloc_report_count((void *)scn, sz);
  SP_HEAP_LOCK();
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
  SP_HEAP_UNLOCK();
  return (char *)h + sizeof(sp_gc_hdr);
}

SP_TLS struct sp_str_lcache_entry sp_str_lcache[SP_STR_LCACHE_SIZE];
SP_TLS void *_sp_ret_strbuf;

void sp_str_lcache_clear(void) {
  for (unsigned i = 0; i < SP_STR_LCACHE_SIZE; i++) sp_str_lcache[i].s = NULL;
}

/* sp_mark_string (sp_gc.h) flips a live string's marker 0xfe->0xfc during the
   mark phase; sweep keeps the marked ones and frees the rest. A frozen heap
   string (0xf1) is kept across sweeps (a live frozen global must survive, and
   frozen literals are immortal). A collectable frozen one goes 0xfa->0xf8 and
   is swept like 0xfe. */
/* Sweep one worker's list head (or the single st list). Runs under stop-the-
   world (threaded) or the held heap lock (st), so no concurrent push races it.
   `bytes` is decremented per freed string to keep the live-byte count in step. */
/* Sweep the YOUNG list: free what the mark phase did not reach, and move every
   survivor onto the old list. The mark reset (0xfc -> 0xfe) happens at the move,
   so a promoted string behaves exactly as it did before -- the next mark phase
   re-marks it if it is still reachable, and the next MAJOR sweep frees it if
   not. Survival of a single sweep is the whole promotion test: a string still
   alive when the young generation filled is, empirically, one the program is
   holding rather than one it is churning through.
   `promoted` accumulates the moved bytes so the caller's threshold retune can
   count them as survivors and not mistake promotion for reclamation. */
/* Sweep one young list into LOCAL results -- the survivors as a list with
   its tail, the bytes they carry, and the bytes the list held in all -- so
   that several young lists of one worker can be swept at once by different
   workers and their results spliced into the slot afterwards
   (sp_str_sweep_young_apply). */
static void sp_str_sweep_young(sp_str_hdr **head, sp_str_hdr **keep_head, sp_str_hdr **keep_tail,
                               size_t *moved_out, size_t *held_out) {
  sp_str_hdr *h = *head;
  sp_str_hdr *keep = NULL, *tail = NULL;
  size_t moved = 0, held = 0;
  while (h) {
    sp_str_hdr *next = h->next;
    SP_PREFETCH(next);
    char *body = (char *)(h + 1);
    unsigned char m = (unsigned char)body[0];
    held += h->size & SP_STR_SIZE_MASK;
    if (m == 0xfc || m == 0xf1 || m == 0xf8) {
      /* Beside the mutators (a sweeper thread) the reset is a compare-and-
         swap: a `freeze` that lands on the same byte in the same moment wins,
         where a plain store could put its 0xfe over the 0xf1. */
      if (m == 0xfc) {
        if (sp_gc_in_sweeper) { unsigned char ex = 0xfc; SP_ATOMIC_CAS((unsigned char *)body, &ex, (unsigned char)0xfe, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
        else body[0] = (char)0xfe;
      }
      else if (m == 0xf8) SP_ATOMIC_STORE((unsigned char *)body, (unsigned char)0xfa, __ATOMIC_RELAXED);
      h->next = keep;
      if (!keep) tail = h;
      keep = h;
      moved += h->size & SP_STR_SIZE_MASK;
    }
    else {
      sp_str_lcache_drop(body + 1);
      sp_slab_free(h);
    }
    h = next;
  }
  *head = NULL;
  *keep_head = keep; *keep_tail = tail;
  *moved_out = moved; *held_out = held;
}
/* Splice one young list's survivors into the slot's old generation and
   settle the counters. Single-threaded per slot: the caller serializes. */
static void sp_str_sweep_young_apply(sp_str_hdr *keep, sp_str_hdr *tail, size_t moved, size_t held,
                                     size_t *bytes, sp_str_hdr **old_head, size_t *old_bytes,
                                     size_t *promoted) {
  if (keep) { tail->next = *old_head; *old_head = keep; }
  *bytes -= held;
  *old_bytes += moved;
  *promoted += moved;
}
static void sp_str_sweep_young_into(sp_str_hdr **head, size_t *bytes,
                                    sp_str_hdr **old_head, size_t *old_bytes,
                                    size_t *promoted) {
  sp_str_hdr *keep, *tail; size_t moved, held;
  sp_str_sweep_young(head, &keep, &tail, &moved, &held);
  sp_str_sweep_young_apply(keep, tail, moved, held, bytes, old_head, old_bytes, promoted);
}

/* ---- Generational verifier, string side (SPINEL_GC_VERIFY_GEN=1) ----
   The object verifier snapshots young OBJECTS a minor mark did not reach and
   re-marks whole-heap to see which of them the full mark does; each one is
   held only through an old object whose barrier is missing. Strings need the
   same check and cannot share that machinery: their mark is a byte on the
   string itself (0xfe unmarked, 0xfc marked), not a generation stamp on a
   header. Snapshot the young strings still unmarked after the minor, then read
   the same byte back after the whole-heap mark. */
static const char **sp_str_vcand = NULL;
static size_t sp_str_vcand_n = 0, sp_str_vcand_cap = 0;
static void sp_str_vcand_push(const char *body) {
  if (sp_str_vcand_n == sp_str_vcand_cap) {
    size_t c = sp_str_vcand_cap ? sp_str_vcand_cap * 2 : 1024;
    const char **n = (const char **)realloc(sp_str_vcand, c * sizeof(const char *));
    if (!n) return;
    sp_str_vcand = n; sp_str_vcand_cap = c;
  }
  sp_str_vcand[sp_str_vcand_n++] = body;
}
static void sp_str_vscan(sp_str_hdr *h) {
  for (; h; h = h->next) {
    const char *body = (const char *)(h + 1);
    if ((unsigned char)body[0] == 0xfe) sp_str_vcand_push(body);
  }
}
/* a slab string's mark is a bit in its chunk, a list string's the byte */
static int sp_str_vmarked(const char *body) {
  const sp_str_hdr *h = ((const sp_str_hdr *)body) - 1;
  if (sp_slab_owns(h)) return sp_slab_is_marked(h);
  return (unsigned char)body[0] == 0xfc;
}
static void sp_str_vunmark(const char *body) {
  const sp_str_hdr *h = ((const sp_str_hdr *)body) - 1;
  if (sp_slab_owns(h)) sp_slab_unmark(h);
  else ((char *)body)[0] = (char)0xfe;
}
static void sp_str_vscan_slab_cb(void *hdr, void *arg) {
  (void)arg;
  const char *body = (const char *)((sp_str_hdr *)hdr + 1);
  if (!sp_str_vmarked(body)) sp_str_vcand_push(body);
}
void sp_str_verify_begin(void) {
  sp_str_vcand_n = 0;
  sp_slab_each_string(1, 0, sp_str_vscan_slab_cb, NULL);
#ifdef SP_THREADS
  { int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
    for (int i = 0; i < n; i++) for (int sub = 0; sub < SP_STR_YSUB; sub++) sp_str_vscan(sp_str_wslot[i].young[sub]); }
#else
  sp_str_vscan(sp_str_heap);
#endif
}
size_t sp_str_verify_end(void) {
  size_t leaked = 0;
  for (size_t i = 0; i < sp_str_vcand_n; i++)
    if (sp_str_vmarked(sp_str_vcand[i])) sp_str_vcand[leaked++] = sp_str_vcand[i];
  sp_str_vcand_n = leaked;   /* keep just the leaked ones, for the holder probe */
  return leaked;
}
/* Holder probe: unmark the leaked strings, let one old object's scan run, and
   see whether it re-marks any. Same shape as the object-side probe, and with
   the same limit -- it names the DIRECT holder, since sp_gc_mark is inert
   while the probe is armed. */
void sp_str_verify_probe_arm(void) {
  for (size_t i = 0; i < sp_str_vcand_n; i++) sp_str_vunmark(sp_str_vcand[i]);
}
int sp_str_verify_probe_hit(void) {
  for (size_t i = 0; i < sp_str_vcand_n; i++)
    if (sp_str_vmarked(sp_str_vcand[i])) return 1;
  return 0;
}
void sp_str_verify_probe_done(void) { sp_str_vcand_n = 0; }

/* Sweep the OLD list in place. Survivors stay old; nothing is demoted. */
static void sp_str_sweep_old(sp_str_hdr **head, size_t *bytes) {
  sp_str_hdr **pp = head;
  while (*pp) {
    sp_str_hdr *h = *pp;
    SP_PREFETCH(h->next);
    char *body = (char *)(h + 1);
    unsigned char m = (unsigned char)body[0];
    if (m == 0xfc) {
      if (sp_gc_in_sweeper) { unsigned char ex = 0xfc; SP_ATOMIC_CAS((unsigned char *)body, &ex, (unsigned char)0xfe, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
      else body[0] = (char)0xfe;
      pp = &h->next;
    }
    else if (m == 0xf1) { pp = &h->next; }
    else if (m == 0xf8) { SP_ATOMIC_STORE((unsigned char *)body, (unsigned char)0xfa, __ATOMIC_RELAXED); pp = &h->next; }
    else {
      *pp = h->next;
      *bytes -= h->size & SP_STR_SIZE_MASK;
      sp_str_lcache_drop(body + 1);
      sp_slab_free(h);
    }
  }
}

/* `major` also walks the old generation. Returns the bytes promoted, for the
   threshold retune. */
static size_t sp_str_sweep_gen(int major) {
  size_t promoted = 0;
#ifdef SP_THREADS
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  for (int i = 0; i < n; i++) {
    if (major) sp_str_sweep_old(&sp_str_wslot[i].old, &sp_str_wslot[i].old_bytes);
    for (int sub = 0; sub < SP_STR_YSUB; sub++)
      sp_str_sweep_young_into(&sp_str_wslot[i].young[sub], &sp_str_wslot[i].young_bytes,
                              &sp_str_wslot[i].old, &sp_str_wslot[i].old_bytes, &promoted);
    /* the young counter also carried the slab strings, swept by the chunk
       sweep of this same stop-the-world cycle: the generation is empty now */
    SP_GC_CTR_SET(sp_str_wslot[i].young_bytes, 0);
    sp_str_wslot[i].ask_at = 0;
  }
#else
  if (major) sp_str_sweep_old(&sp_str_old, &sp_str_old_bytes);
  sp_str_sweep_young_into(&sp_str_heap, &sp_str_heap_bytes,
                          &sp_str_old, &sp_str_old_bytes, &promoted);
  SP_GC_CTR_SET(sp_str_heap_bytes, 0);
#endif
  return promoted;
}

/* Full sweep of both generations. GC.start and the shutdown paths want every
   unreachable string gone, not just the young ones. */
void sp_str_sweep(void) {
  (void)sp_str_sweep_gen(1);
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
  if (sp_polyarr_pool_count >= SP_POLYARR_POOL_MAX || a->cap > SP_POLYARR_POOL_KEEP_CAP) {
    if (a->data != a->inl) sp_pl_free(a->data);
    sp_slab_free(h);
    return;
  }
  h->next = sp_polyarr_pool_head;
  sp_polyarr_pool_head = h;
  sp_polyarr_pool_count++;
}

/* String sweep, gated on the string heap's own trigger. The object collector
   used to run the full live-string walk on EVERY collection, making each one
   O(live strings) -- the dominant cost of allocation-heavy programs (#2922
   profiling on BabyStark: 2.9s of an 8.0s GC total). Skipping is safe:
   string marks accumulate, so a dead string at worst survives until the next
   string sweep (delayed reclamation, not a leak); the sweep itself resets
   marks for the next cycle. Retuning here (with the collector-side retunes
   removed) keeps the trigger tracking the live size in one place. */
/* The gate, split so the per-worker middle can run on the workers themselves.
   `begin` decides (and remembers `before` for the retune), `one` sweeps one
   worker's two lists, `end` re-aims the thresholds. The serial driver below
   still calls all three in a row; the scheduler interleaves the middle across
   the parked workers instead. */
static size_t sp_str_gate_before = 0;
#ifdef SP_THREADS
int sp_str_par_done = 0;   /* the workers already did it for this collection */
#endif
/* The decision sp_str_sweep_begin will make this cycle, without its side
   effects (the cycle counter, the shape sample): over the trigger, and a major
   on schedule or forced by the old generation's growth. */
static int sp_str_major_due(void) {
#ifdef SP_THREADS
  size_t before = sp_str_bytes_total();
#else
  size_t before = SP_GC_CTR_GET(sp_str_heap_bytes);
#endif
  if (before <= SP_GC_CTR_GET(sp_str_threshold)) return 0;
  if (sp_str_old_total() > sp_str_old_threshold) return 1;
  if (!sp_gc_str_major_sched) return 0;
  return (sp_str_sweep_cycle % (unsigned)sp_str_major_interval) == 0;
}
int sp_str_sweep_begin(int *major) {
#ifdef SP_THREADS
  size_t before = sp_str_bytes_total();
#else
  size_t before = SP_GC_CTR_GET(sp_str_heap_bytes);
#endif
  if (before <= SP_GC_CTR_GET(sp_str_threshold)) return 0;
  sp_str_gate_before = before;
  sp_str_gate_old = sp_str_old_total();
  /* Only once a major has run: before that the minimum is the empty heap, not
     "what a major can leave", and a min of zero makes the ratio meaningless
     (it reads 0.0x rather than large). */
  if (sp_gc_str_majors > 0) {
    sp_str_shape[sp_str_shape_n % SP_STR_SHAPE_MAX] = sp_str_gate_old;
    sp_str_shape_n++;
  }
  /* Walk the old generation only once it has itself grown past a threshold,
     then re-aim that threshold at what survived. Between majors, old strings
     that die are reclaimed late -- the same delayed-reclamation trade this
     gate already makes for the whole heap, one level up. */
  /* On schedule, or forced by growth the schedule did not keep up with. The
     forced arm is what the size test used to be on its own; behind a schedule
     it is a backstop, which is the whole difference. */
  if (!sp_gc_str_major_sched) { *major = sp_str_old_total() > sp_str_old_threshold; }
  else {
    int sched = (sp_str_sweep_cycle % (unsigned)sp_str_major_interval) == 0;
    sp_str_sweep_cycle++;
    sp_str_major_forced = 0;
    if (!sched && sp_str_old_total() > sp_str_old_threshold) {
      sched = 1; sp_str_major_forced = 1;
    }
    *major = sched;
  }
  return 1;
}
void sp_str_sweep_end_excluding(int major, size_t promoted, size_t young_exclude) {
  sp_str_retune_young_exclude = young_exclude;
  sp_str_sweep_end(major, promoted);
  sp_str_retune_young_exclude = 0;
}
void sp_str_sweep_end(int major, size_t promoted) {
  /* the slab strings this cycle's mark promoted joined old too (their
     generation is swept every cycle, gate or no gate; sp_str_mark_settle) */
  promoted += sp_gc_mk_str_young_bytes;
  if (major) {
    sp_gc_str_majors++;
    size_t old_after = sp_str_old_total();
    /* SPINEL_GC_STR_MAJOR=fixed holds both the backstop and the cadence where
       the floor put them, which is what makes a policy measurable against
       itself. SPINEL_GC_STR_MAJOR=size turns the schedule off entirely and
       leaves the size test as the whole policy, which is what shipped before. */
    /* The old set this major walked is what it KEPT: sp_str_old_total() after
       the sweep also carries what the same sweep promoted out of young, and
       that is not a survivor of anything yet. Both re-aims below are taken
       over kept, for the reason the ratio's note gives. */
    size_t kept = old_after > promoted ? old_after - promoted : 0;
    if (!sp_gc_str_major_fixed) {
      /* Re-baseline the backstop: twice what this major KEPT, the bound
         sp_gc_collect keeps for the object old generation. Aiming it at
         old_after instead let the strings a request had in flight at the sweep
         set the gate: on Campfire every sweep promotes 50-70 MB of them, they
         die milliseconds later, and a gate of twice kept-plus-promoted held
         three sweeps of that garbage (old 290 MB against 20 MB kept) -- and the
         young trigger, which retunes on the whole string heap, followed it up
         to 12 MB a worker. Aimed at kept, the next sweep whose promotion
         outgrows the live set is a major, and the old generation stays within
         one sweep's promotion of what is live. */
      sp_str_old_threshold = sp_gc_sat_mul(kept, 2);
      if (sp_str_old_threshold < sp_str_old_threshold_init)
        sp_str_old_threshold = sp_str_old_threshold_init;
      /* Adapt the CADENCE from the survival RATIO. A ratio is scale-free, so
         unlike a size it cannot carry the last major's inflation into the next
         one. A major FORCED by the backstop is not a sample taken on schedule:
         growth that is still live reads as ~100% survival and would lengthen
         the cadence that was already too short to hold it. So a forced major
         shortens and does not adapt -- sp_gc.c says this for the object heap,
         and said it first. */
      if (!sp_gc_str_major_sched) { /* the size gate is the whole policy */ }
      else if (sp_str_major_forced) {
        if (sp_str_major_interval > SP_STR_MAJOR_INTERVAL) sp_str_major_interval /= 2;
      }
      else if (sp_str_gate_old > 0) {
        /* The ratio has to be taken over the OLD SET THIS MAJOR WALKED, and
           sp_str_old_total() is not that: it already carries what this same
           sweep promoted out of young (the note in sp_str_retune says so for
           the same reason). Counting promotions as survivors reads a healthy
           reclamation as ~100% survival and lengthens the cadence -- the size
           gate's contamination, arriving a second time in ratio form. Measured
           at a 4 MB floor it walked the interval up to 32 and left 61 MB of old
           against a 12 MB live set. */
        size_t before = sp_str_gate_old;
        if (kept > before - (before >> 2)) {                /* >75% survived */
          if (sp_str_major_interval < SP_STR_MAJOR_INTERVAL_MAX) sp_str_major_interval *= 2;
        }
        else if (kept < (before >> 1)) {                   /* <50% survived */
          if (sp_str_major_interval > SP_STR_MAJOR_INTERVAL) sp_str_major_interval /= 2;
        }
      }
    }
  }
  sp_str_retune(sp_str_gate_before, promoted);
}
#ifdef SP_THREADS
/* One worker's own string lists. Freeing a string on the worker that allocated
   it keeps the block in the arena it came from: the collector doing all eight
   workers' frees turned every one of them into a cross-arena free, which is
   the slow path in glibc and in every other thread-caching allocator. Each
   worker also clears its own length cache, whose entries are keyed by the
   addresses this sweep is about to recycle. */
/* The list forms, for the concurrent driver (sp_sched.c): the lists were
   detached from their slots under the barrier, so they arrive as plain
   pointers, and the old sweep answers the bytes it freed for the slot's
   counter to take at the next barrier. */
void sp_str_sweep_young_list(sp_str_hdr **head, sp_str_hdr **keep, sp_str_hdr **tail, size_t *moved, size_t *held) {
  sp_str_sweep_young(head, keep, tail, moved, held);
}
size_t sp_str_sweep_old_list(sp_str_hdr **head) {
  size_t bytes = (size_t)-1 / 2;   /* a counter the sweep decrements; the difference is what it freed */
  size_t start = bytes;
  sp_str_sweep_old(head, &bytes);
  return start - bytes;
}
/* The sweep tasks of one slot, for the parallel driver (sp_sched.c): the old
   list on a major, and each young list into local results that
   sp_str_sweep_young_done splices in once every task of the slot is over. */
void sp_str_sweep_old_one(int wid) {
  sp_str_sweep_old(&sp_str_wslot[wid].old, &sp_str_wslot[wid].old_bytes);
}
void sp_str_sweep_young_one(int wid, int sub, sp_str_hdr **keep, sp_str_hdr **tail,
                            size_t *moved, size_t *held) {
  sp_str_sweep_young(&sp_str_wslot[wid].young[sub], keep, tail, moved, held);
}
void sp_str_sweep_young_done(int wid, sp_str_hdr *keep, sp_str_hdr *tail, size_t moved, size_t held,
                             size_t *promoted) {
  sp_str_sweep_young_apply(keep, tail, moved, held, &sp_str_wslot[wid].young_bytes,
                           &sp_str_wslot[wid].old, &sp_str_wslot[wid].old_bytes, promoted);
}
#endif
static void sp_str_sweep_gated(void) {
#ifdef SP_THREADS
  if (sp_str_par_done) { sp_str_par_done = 0; return; }
#endif
  int major = 0;
  if (!sp_str_sweep_begin(&major)) return;
  /* the old list holds the strings the minor mark could not reach */
  if (sp_gc_str_minor_only) major = 0;
  size_t promoted = sp_str_sweep_gen(major);
  sp_str_sweep_end(major, promoted);
}

/* Non-inline sp_str_alloc, for a TU that cannot include sp_alloc.h.
   lib/sp_bigint.c is the one: it pulls mruby_shim.h, whose sp_bool disagrees
   with sp_types.h's, so the header cannot be added alongside. Its Integer#to_s
   still has to answer a string-heap string like every other producer (#3396). */
char *sp_str_alloc_ext(size_t len) { return sp_str_alloc(len); }

/* Wire string sweep into the object collector. Runs before main, so the hook is
   set before the first allocation can trigger a collection. */
SP_CONSTRUCTOR static void sp_alloc_install_hooks(void) {
  sp_gc_str_sweep_hook = sp_str_sweep_gated;
  sp_gc_str_major_due_hook = sp_str_major_due;
  sp_gc_obj_retune_hook = sp_gc_retune_object;
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
/* A site's human name, resolved at dump time. Prefers the symbol name from
   the dynamic symbol table; falls back to the raw address. Caller frees. */
static char *sp_alloc_site_name(void *site) {
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
