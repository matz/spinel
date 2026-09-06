/* sp_alloc.c -- the single, shared definitions backing sp_alloc.h.

   Owns the string heap so that both the generated program and every standalone
   lib/*.c allocate onto one heap. sp_str_sweep is registered with the object GC
   via a constructor, so a collection triggered from any TU also reaps strings. */
#include <time.h>
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
static size_t sp_str_bytes_total(void) {
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
size_t sp_str_old_threshold = 1024 * 1024;
size_t sp_str_old_threshold_init = 1024 * 1024;

/* Live bytes in the old generation, across every worker's list. */
static size_t sp_str_old_total(void) {
#ifdef SP_THREADS
  size_t t = 0;
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  for (int i = 0; i < n; i++) t += SP_GC_CTR_GET(sp_str_wslot[i].old_bytes);
  return t;
#else
  return SP_GC_CTR_GET(sp_str_old_bytes);
#endif
}
size_t sp_str_threshold = 256 * 1024;
size_t sp_str_threshold_init = 256 * 1024;
int sp_str_stress_checked = 0;

const char sp_str_empty_data[] = "\xff";

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
void sp_alloc_worker_tune(int workers) {
  const char *e = getenv("SPINEL_GC_THRESHOLD_KB");
  if (e && *e) {
    long v = atol(e);
    if (v > 0) {
      size_t base = (size_t)v * 1024;
      SP_GC_CTR_SET(sp_gc_threshold, base); sp_gc_threshold_init = base;
      SP_GC_CTR_SET(sp_str_threshold, base); sp_str_threshold_init = base;
    }
  }
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
          "live %.1f MB obj + %.1f MB str; trigger %.1f MB obj + %.2f MB str/worker x %d\n",
          n, sp_gc_stat_fulls, sp_gc_stat_seconds, wall,
          wall > 0 ? 100.0 * sp_gc_stat_seconds / wall : 0.0,
          n ? 1000.0 * sp_gc_stat_seconds / (double)n : 0.0,
          (double)SP_GC_CTR_GET(sp_gc_bytes) / 1048576.0, (double)sbytes / 1048576.0,
          (double)SP_GC_CTR_GET(sp_gc_threshold) / 1048576.0,
          (double)SP_GC_CTR_GET(sp_str_threshold) / 1048576.0, nw);
}

void sp_gc_retune_object(size_t before) {
  sp_gc_stats_report();
  if (sp_gc_stress_pin) { sp_gc_threshold = sp_gc_threshold_init; return; }
  size_t live = sp_gc_bytes;
  /* saturating: the live counter is a heuristic and is allowed to lag, so it
     can read above the pre-collect total. Wrapping made `freed` enormous, the
     productive-sweep test went false, and the threshold was taken from a live
     count that had itself wrapped. */
  size_t freed = before > live ? before - live : 0;
  if (freed < before / 4) { sp_gc_threshold = sp_gc_sat_mul(before, 2); }
  else if (live > 0) { sp_gc_threshold = sp_gc_sat_mul(live, 2); if (sp_gc_threshold < sp_gc_threshold_init) sp_gc_threshold = sp_gc_threshold_init; }
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
static void sp_str_retune(size_t before, size_t promoted) {
  if (sp_gc_stress_pin) { sp_str_threshold = sp_str_threshold_init; return; }
#ifdef SP_THREADS
  int nw = sp_active_workers; if (nw < 1) nw = 1;
  size_t after = (sp_str_bytes_total() + sp_str_old_total()) / (size_t)nw;
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

void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
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
  sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, need);
  if (!h) sp_oom_die();
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0; h->old = 0; h->dirty = 0;
  if (sp_alloc_report_on) sp_alloc_report_count((void *)scn, sz);
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
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
  sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, need);
  if (!h) sp_oom_die();
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0; h->old = 0; h->dirty = 0;
  if (sp_alloc_report_on) sp_alloc_report_count((void *)scn, sz);
  SP_GC_HEAP_PUSH(h); sp_gc_bytes_add(need);
  SP_HEAP_UNLOCK();
  return (char *)h + sizeof(sp_gc_hdr);
#endif
}
void *sp_gc_alloc_nogc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, need);
  if (!h) sp_oom_die();
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0; h->old = 0; h->dirty = 0;
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
   frozen literals are immortal). */
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
static void sp_str_sweep_young(sp_str_hdr **head, size_t *bytes,
                               sp_str_hdr **old_head, size_t *old_bytes,
                               size_t *promoted) {
  sp_str_hdr *h = *head;
  sp_str_hdr *keep = *old_head;
  size_t moved = 0;
  while (h) {
    sp_str_hdr *next = h->next;
    char *body = (char *)(h + 1);
    unsigned char m = (unsigned char)body[0];
    if (m == 0xfc || m == 0xf1) {
      if (m == 0xfc) body[0] = (char)0xfe;
      h->next = keep;
      keep = h;
      moved += h->size & SP_STR_SIZE_MASK;
    }
    else {
      *bytes -= h->size & SP_STR_SIZE_MASK;
      sp_str_lcache_drop(body + 1);
      free(h);
    }
    h = next;
  }
  *head = NULL;
  *old_head = keep;
  *bytes -= moved;
  *old_bytes += moved;
  *promoted += moved;
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
void sp_str_verify_begin(void) {
  sp_str_vcand_n = 0;
#ifdef SP_THREADS
  { int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
    for (int i = 0; i < n; i++) sp_str_vscan(sp_str_wslot[i].young); }
#else
  sp_str_vscan(sp_str_heap);
#endif
}
size_t sp_str_verify_end(void) {
  size_t leaked = 0;
  for (size_t i = 0; i < sp_str_vcand_n; i++)
    if ((unsigned char)sp_str_vcand[i][0] == 0xfc) sp_str_vcand[leaked++] = sp_str_vcand[i];
  sp_str_vcand_n = leaked;   /* keep just the leaked ones, for the holder probe */
  return leaked;
}
/* Holder probe: unmark the leaked strings, let one old object's scan run, and
   see whether it re-marks any. Same shape as the object-side probe, and with
   the same limit -- it names the DIRECT holder, since sp_gc_mark is inert
   while the probe is armed. */
void sp_str_verify_probe_arm(void) {
  for (size_t i = 0; i < sp_str_vcand_n; i++) ((char *)sp_str_vcand[i])[0] = (char)0xfe;
}
int sp_str_verify_probe_hit(void) {
  for (size_t i = 0; i < sp_str_vcand_n; i++)
    if ((unsigned char)sp_str_vcand[i][0] == 0xfc) return 1;
  return 0;
}
void sp_str_verify_probe_done(void) { sp_str_vcand_n = 0; }

/* Sweep the OLD list in place. Survivors stay old; nothing is demoted. */
static void sp_str_sweep_old(sp_str_hdr **head, size_t *bytes) {
  sp_str_hdr **pp = head;
  while (*pp) {
    sp_str_hdr *h = *pp;
    char *body = (char *)(h + 1);
    unsigned char m = (unsigned char)body[0];
    if (m == 0xfc) { body[0] = (char)0xfe; pp = &h->next; }
    else if (m == 0xf1) { pp = &h->next; }
    else {
      *pp = h->next;
      *bytes -= h->size & SP_STR_SIZE_MASK;
      sp_str_lcache_drop(body + 1);
      free(h);
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
    sp_str_sweep_young(&sp_str_wslot[i].young, &sp_str_wslot[i].young_bytes,
                       &sp_str_wslot[i].old, &sp_str_wslot[i].old_bytes, &promoted);
  }
#else
  if (major) sp_str_sweep_old(&sp_str_old, &sp_str_old_bytes);
  sp_str_sweep_young(&sp_str_heap, &sp_str_heap_bytes,
                     &sp_str_old, &sp_str_old_bytes, &promoted);
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
   heap-byte accounting change hands. */
sp_gc_hdr *sp_polyarr_pool_head = NULL;
long sp_polyarr_pool_count = 0;
#define SP_POLYARR_POOL_MAX 65536
#define SP_POLYARR_POOL_KEEP_CAP 64   /* don't retain unusually large buffers */
void sp_PolyArray_pool_recycle(sp_gc_hdr *h) {
  sp_PolyArray *a = (sp_PolyArray *)((char *)h + sizeof(sp_gc_hdr));
  long n;
#ifdef SP_THREADS
  n = __atomic_load_n(&sp_polyarr_pool_count, __ATOMIC_RELAXED);
#else
  n = sp_polyarr_pool_count;
#endif
  if (n >= SP_POLYARR_POOL_MAX || a->cap > SP_POLYARR_POOL_KEEP_CAP) {
    free(a->data);
    free(h);
    return;
  }
#ifdef SP_THREADS
  sp_gc_hdr *old;
  do { old = __atomic_load_n(&sp_polyarr_pool_head, __ATOMIC_ACQUIRE); h->next = old;
  } while (!__atomic_compare_exchange_n(&sp_polyarr_pool_head, &old, h,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  __atomic_fetch_add(&sp_polyarr_pool_count, 1, __ATOMIC_RELAXED);
#else
  h->next = sp_polyarr_pool_head;
  sp_polyarr_pool_head = h;
  sp_polyarr_pool_count++;
#endif
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
int sp_str_sweep_begin(int *major) {
#ifdef SP_THREADS
  size_t before = sp_str_bytes_total();
#else
  size_t before = SP_GC_CTR_GET(sp_str_heap_bytes);
#endif
  if (before <= SP_GC_CTR_GET(sp_str_threshold)) return 0;
  sp_str_gate_before = before;
  sp_str_gate_old = sp_str_old_total();
  /* Walk the old generation only once it has itself grown past a threshold,
     then re-aim that threshold at what survived. Between majors, old strings
     that die are reclaimed late -- the same delayed-reclamation trade this
     gate already makes for the whole heap, one level up. */
  *major = sp_str_old_total() > sp_str_old_threshold;
  return 1;
}
void sp_str_sweep_end(int major, size_t promoted) {
  if (major) {
    size_t old_after = sp_str_old_total();
    sp_str_old_threshold = old_after * 2;
    if (sp_str_old_threshold < sp_str_old_threshold_init)
      sp_str_old_threshold = sp_str_old_threshold_init;
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
void sp_str_sweep_one(int wid, int major, size_t *promoted) {
  if (major) sp_str_sweep_old(&sp_str_wslot[wid].old, &sp_str_wslot[wid].old_bytes);
  sp_str_sweep_young(&sp_str_wslot[wid].young, &sp_str_wslot[wid].young_bytes,
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
__attribute__((constructor)) static void sp_alloc_install_hooks(void) {
  sp_gc_str_sweep_hook = sp_str_sweep_gated;
  sp_gc_obj_retune_hook = sp_gc_retune_object;
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
void sp_alloc_report_count(void *scan, size_t bytes) {
  sp_AllocStat *s = sp_alloc_stat_slot(scan ? scan : SP_ALLOC_NOSCAN_KEY, sp_alloc_site_now());
  s->count++; s->bytes += (unsigned long long)bytes;
}
void sp_alloc_report_str(size_t bytes) {
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
__attribute__((constructor)) static void sp_alloc_report_boot(void) {
  const char *e = getenv("SPINEL_ALLOC_REPORT");
  if (e && *e && strcmp(e, "0") != 0) {
    sp_alloc_report_on = 1;
    { const char *sv = getenv("SPINEL_ALLOC_SITES");
      sp_alloc_sites_on = (sv && *sv && strcmp(sv, "0") != 0) ? 1 : 0; }
    atexit(sp_alloc_report_dump);
  }
}
