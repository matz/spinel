/* sp_sched.c -- cooperative M:N thread scheduler bodies, Phase 0 (N=1).
 * See sp_sched.h. Built on the sp_fiber context switch: the main thread runs on
 * the root fiber and pumps a run queue of green threads whenever it blocks. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* sched_getaffinity / CPU_COUNT for the worker count */
#endif
#include "sp_sched.h"
#ifdef SP_CEXT
#include "sp_cext.h"
#endif
#include "sp_alloc.h"   /* sp_box_nil / sp_box_obj */
#include "sp_exc.h"     /* sp_exc_cls_display / sp_exc_cls_unnamed (the thread report) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>     /* sysconf (worker count) */
#ifdef __linux__
#include <sched.h>      /* sched_getaffinity (worker count) */
#endif
#include <time.h>       /* clock_gettime (Kernel#sleep) */
#include <errno.h>      /* EINTR (sleep fallback) */
#ifdef __linux__
#include <sys/prctl.h>  /* PR_SET_NAME (the sweeper threads' name) */
#endif
#include <sys/wait.h>   /* waitpid (sp_sched_wait_child) */
#include <signal.h>     /* preemption signal (SIGURG by default) */
#include <strings.h>    /* strcasecmp (SPINEL_PREEMPT_SIGNAL by name) */
#include <stdint.h>     /* intptr_t (worker id passed via pthread arg) */
#include <poll.h>       /* poll (scheduler-aware I/O) */
#include <fcntl.h>      /* fcntl O_NONBLOCK (monitor wake pipe) */

/* Which readiness backend this build has, decided BEFORE anything that tests
   it. It used to be declared beside the registration table, halfway down --
   below the wake helpers, whose `#ifdef SP_EV_BACKEND` arms were therefore
   compiled out. Every cross-worker kick vanished with them, and only the
   backstop timeout was left to deliver a wake: the ping-pong ran at full speed
   on one worker (which needs no kick) and at a twentieth of it on two. */
#if defined(SP_THREADS) && defined(__linux__)
#define SP_EV_BACKEND 1
#define SP_EV_EPOLL 1
#include <sys/epoll.h>
#elif defined(SP_THREADS) && (defined(__APPLE__) || defined(__FreeBSD__) || \
                              defined(__OpenBSD__) || defined(__NetBSD__))
/* kqueue keys by (descriptor, filter), so READ and WRITE are two entries for
   one fd; EV_ONESHOT DELETES the entry after its delivery where epoll only
   disables it, which makes the re-arm a plain EV_ADD. Everything above the
   three backend calls is the same on both. */
#define SP_EV_BACKEND 1
#define SP_EV_KQUEUE 1
#include <sys/types.h>   /* <sys/event.h> wants it first on the BSDs */
#include <sys/event.h>
#endif
#ifdef SP_EV_BACKEND
static void sp_ev_kick(int wid);
#endif

/* Reached by name (defined in lib/sp_alloc.c or the generated TU), exactly as
   lib/sp_fiber.c reaches them. */
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
void sp_re_push_match_roots(void);   /* lib/sp_re.c: STW match-register publishing */
SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);
/* Per-context exception handler stack (defined in the generated TU). */
void *sp_exc_ctx_new(void);
void  sp_exc_ctx_save(void *p);
void  sp_exc_ctx_load(void *p);
void  sp_exc_ctx_free(void *p);

/* Safepoint (design 5.1). Set while a collector wants the world stopped; polled
   at loop back-edges (codegen) and at blocking points. Defined unconditionally
   (see sp_sched.h) so a threaded program's poll links against either archive;
   the flag is only ever set in the threaded build. sp_safepoint() parks the
   worker at the GC barrier (defined below, after the lock). At N=1 the flag is
   never set by another worker, so a single worker never parks here. */
volatile int sp_safepoint_flag = 0;
void (*sp_safepoint_publish_hook)(void) = NULL;   /* set by the generated TU (sp_sched.h) */

/* ---- scheduler lock (design 3, Appendix B) ----
 * One mutex guards the scheduler metadata that is global: the global run queue,
 * the timer heap, the Queue, sleep and I/O wait lists and the Queue ring
 * buffers. What a hand-off between running threads touches has locks of its
 * own since #8702: each worker's run queue (rq_lock), each Mutex's,
 * ConditionVariable's and thread's joiners list (a wait lock, sp_wlock), and the
 * live-thread registry (g_reg_lock). A worker holds the scheduler lock only while touching that metadata
 * and ALWAYS drops it across a fiber transfer (running a green thread or parking
 * itself). The held region therefore never polls a safepoint, allocates from the
 * GC heap, or runs Ruby -- which is what lets a stop-the-world collector make
 * progress: a worker blocked on this lock releases it within a bounded critical
 * section and then reaches its next safepoint. Internal helpers (rq_*, reg_*,
 * sp_sched_wake_one, sp_sched_unpark, sp_thread_wake_joiners) run with the lock
 * already held by their caller; sp_sched_block / sp_sched_pump / sp_sched_pass
 * are entered and left with the lock held, bracketing their transfers. In the
 * single-threaded archive the macros are no-ops, so that build is byte-identical
 * and the N=1 path is unchanged save for the (uncontended) lock calls. */
#ifndef SP_MAX_WORKERS
#define SP_MAX_WORKERS 256   /* both builds: sizes the per-worker run-queue array */
#endif
/* The monotonic clock, outside the SP_THREADS guard: the monitor uses it for
   deadlines, and so does Thread#join(limit), which is compiled into both
   builds because the codegen emits its symbol unconditionally. */
static double sp_monotonic_now(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#ifdef SP_THREADS
#include <pthread.h>
static pthread_mutex_t g_sched_lock = PTHREAD_MUTEX_INITIALIZER;
/* (#8702) whether this thread holds g_sched_lock, so a wake that
   has to go through it (main's worker) knows whether to take it. */
static SP_TLS int g_sched_held = 0;
#define SCHED_LOCK()    (pthread_mutex_lock(&g_sched_lock), g_sched_held = 1)
#define SCHED_UNLOCK()  (g_sched_held = 0, pthread_mutex_unlock(&g_sched_lock))
/* each worker's local run queue and its idle flag have a lock of their
   own, so a wake can push a thread to its home worker without the scheduler
   lock. Order: g_sched_lock -> a wait lock -> a run-queue lock. */
/* One cache line each: packed, neighbouring workers' locks shared lines, and
   every push and pop on one worker took its neighbours' lines away. */
typedef struct { pthread_mutex_t m; } __attribute__((aligned(64))) sp_rq_lock;
static sp_rq_lock g_rq_lock[SP_MAX_WORKERS] = { [0 ... SP_MAX_WORKERS - 1] = { PTHREAD_MUTEX_INITIALIZER } };
#define RQ_LOCK(w)   pthread_mutex_lock(&g_rq_lock[w].m)
#define RQ_UNLOCK(w) pthread_mutex_unlock(&g_rq_lock[w].m)
/* The live-thread registry (g_all) has a lock of its own, so a thread's death
   leaves it without the scheduler lock. Nothing is taken while it is held. */
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
#define REG_LOCK()   pthread_mutex_lock(&g_reg_lock)
#define REG_UNLOCK() pthread_mutex_unlock(&g_reg_lock)
/* A short spin, then a yield: with fewer cores than workers the holder can be
   descheduled inside its few instructions, and spinning on then only keeps it
   off the CPU (2 cores, 8 workers: a pool round 24% slower than master with a
   pure spin). macOS's sched_yield also lowers the caller's priority, so the
   yield there is thread_switch. */
#if defined(__APPLE__)
#include <mach/thread_switch.h>
#define SP_WL_YIELD() ((void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0))
#else
#define SP_WL_YIELD() sched_yield()
#endif
static inline void wl_lock(sp_wlock *l) {
  int spins = 0;
  while (SP_ATOMIC_EXCHANGE(l, 1, __ATOMIC_ACQUIRE))
    while (SP_ATOMIC_LOAD(l, __ATOMIC_RELAXED)) {
      if (++spins < 64) SP_CPU_RELAX();
      else { spins = 0; SP_WL_YIELD(); }
    }
}
static inline void wl_unlock(sp_wlock *l) { SP_ATOMIC_STORE(l, 0, __ATOMIC_RELEASE); }

/* ---- stop-the-world GC barrier (design 6.2) ----
 * All state guarded by g_sched_lock. g_stw_active is the authoritative
 * "collection in progress" predicate; sp_safepoint_flag is a lock-free hint the
 * codegen polls at loop back-edges so a running worker checks in cheaply. The
 * triggering worker (over the alloc threshold) either becomes the sole collector
 * or, if one is already running, parks like everyone else -- there is no
 * separate collector lock to block on, so a worker that crossed the threshold
 * can never stall the collector by being un-parkable. */
int                   sp_active_workers = 1;   /* exported: worker count */   /* worker count; C-3b raises it past 1 */
static int            g_nparked  = 0;   /* workers parked at the barrier right now */
/* The fiber each parked worker was running when it published its roots (a green
   thread, or the worker's root fiber for an idle/main worker). The collector
   marks these: green-thread fibers are also reached via sp_fiber_list_head, but a
   worker's root fiber is not on that list, so without this a helper collector
   would miss the main thread's top-level roots. */
static sp_Fiber      *g_parked_fiber[2 * SP_MAX_WORKERS];   /* up to 2 per worker: current + root */
static int            g_n_parked_fiber = 0;
static int            g_stw_active = 0; /* a collection is in progress */
/* Parallel sweep phase, inside the stop-the-world window. The workers are
   already parked here doing nothing while the collector sweeps, and the sweep
   is 93% of the stopped time -- so hand each of them its OWN slot instead.
   Its own, not any slot: freeing an object returns it to the arena it was
   allocated from, and a single thread freeing eight workers' objects pays for
   eight arena locks and eight cold metadata sets. */
static int            g_sweep_go = 0;                 /* collector wants the tasks run */
static pthread_cond_t g_sweep_cv = PTHREAD_COND_INITIALIZER;
/* survivors, per slot, spliced onto the shared old heap by the collector */
static sp_gc_hdr     *g_sw_head[SP_MAX_WORKERS];
static sp_gc_hdr     *g_sw_tail[SP_MAX_WORKERS];
static size_t         g_sw_bytes[SP_MAX_WORKERS];
/* Decided once per collection by sp_sched_par_sweep, read by every worker. */
static int    g_str_sweep = 0;
static int    g_str_major = 0;
static size_t g_str_promoted[SP_MAX_WORKERS];
/* The sweep is a list of TASKS, not a slot per worker. One busy worker's
   young lists are the bulk of most cycles on a server (a request handler
   allocates; the others idle), and with a slot per worker that one worker
   swept alone while the rest waited: the longest slot WAS the phase, four
   milliseconds of thirty workers idle. So a slot is several tasks -- its
   object list, its old string list on a major, and each of its SP_STR_YSUB
   young string lists -- and every parked worker claims tasks off a counter
   until none is left. The tasks of one slot touch distinct lists; what they
   share (the slot's old string head and byte counters) is settled by the
   collector afterwards from the per-task results. */
enum { SW_OBJ, SW_STR_OLD, SW_STR_YOUNG, SW_CHUNKS };
typedef struct { short kind, wid, sub; } sp_sw_task;
#define SW_TASK_MAX (SP_MAX_WORKERS * (3 + SP_STR_YSUB))
static sp_sw_task     g_sw_tasks[SW_TASK_MAX];
static int            g_sw_ntasks = 0;
static int            g_sw_next = 0;    /* claimed by fetch-add, off the lock */
static int            g_sw_done = 0;    /* completed, under the lock */
/* per (slot, young list): the sweep's local results, spliced in by the collector */
static sp_str_hdr    *g_sy_keep[SP_MAX_WORKERS][SP_STR_YSUB];
static sp_str_hdr    *g_sy_tail[SP_MAX_WORKERS][SP_STR_YSUB];
static size_t         g_sy_moved[SP_MAX_WORKERS][SP_STR_YSUB];
static size_t         g_sy_held[SP_MAX_WORKERS][SP_STR_YSUB];
/* SPINEL_GC_PHASES: the longest single task of each sweep, summed, beside
   the phase's wall. The gap between the two is the cost of driving the
   parallel phase itself (waking the parked workers and collecting their
   reports), which is what to look at when the phase is long and the tasks
   are not. */
static unsigned long g_sw_slot_max_us = 0;
static unsigned long g_sw_task_sum_us = 0;   /* every task's time added: the work the phase spread */
static unsigned long g_sw_task_max_kind[4];  /* the kind of the longest task, for the report */
void sp_gc_sweep_chunks_slot(int wid);   /* lib/sp_gc.c: the slab bitmaps of one slot, this cycle */
static void sp_sweep_task(const sp_sw_task *t) {
  double t0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  switch (t->kind) {
    case SW_OBJ: sp_gc_sweep_slot(t->wid, &g_sw_head[t->wid], &g_sw_tail[t->wid], &g_sw_bytes[t->wid]); break;
    case SW_CHUNKS: sp_gc_sweep_chunks_slot(t->wid); break;
    case SW_STR_OLD: sp_str_sweep_old_one(t->wid); break;
    default: sp_str_sweep_young_one(t->wid, t->sub, &g_sy_keep[t->wid][t->sub], &g_sy_tail[t->wid][t->sub],
                                    &g_sy_moved[t->wid][t->sub], &g_sy_held[t->wid][t->sub]); break;
  }
  if (sp_gc_ph_on) {
    /* microseconds in an integer, so the max is one atomic */
    unsigned long d = (unsigned long)((sp_monotonic_now() - t0) * 1e6), m;
    SP_ATOMIC_FETCH_ADD(&g_sw_task_sum_us, d, __ATOMIC_RELAXED);
    SP_ATOMIC_FETCH_ADD(&g_sw_task_max_kind[t->kind], d, __ATOMIC_RELAXED);
    do { m = SP_ATOMIC_LOAD(&g_sw_slot_max_us, __ATOMIC_RELAXED); if (d <= m) break; }
    while (!SP_ATOMIC_CAS(&g_sw_slot_max_us, &m, d, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  }
}
/* The parallel mark: parked workers lent to the collector's drain. */
static int      g_mk_go = 0;          /* a drain wants helpers */
static unsigned g_mk_gen = 0;         /* one per drain; a worker helps a drain once */
static int      g_mk_joined = 0;      /* helpers that took a seat this drain */
static int      g_mk_active = 0;      /* helpers still inside the drain */
static int      g_mk_max = 0;         /* seats: SPINEL_GC_MARKERS, default min(cores, 8) */
static pthread_cond_t g_mk_cv = PTHREAD_COND_INITIALIZER;   /* the collector waits for active == 0 */
static SP_TLS unsigned g_mk_seen = 0;
static int g_cs_help = 0;           /* parked workers may claim the concurrent sweep's tasks */
static int g_cs_ntasks = 0, g_cs_finished = 0;   /* the concurrent sweep's task list (below) */
static int g_cs_running = 0;                     /* sweeper threads and barrier helpers inside the task list */
extern double sp_gc_ph_park_sweeping; extern unsigned long long sp_gc_ph_park_sweeping_n;   /* sp_gc.c */
static int g_cs_unclaimed = 0;                   /* tasks nobody has taken yet */
static void sp_cs_help_run(void);
static void sp_cs_owner_run(int wid);
static int g_cs_full;   /* the running concurrent sweep is a full cycle's (defined with its siblings below) */
static int g_cs_owner_env = -1;                  /* SPINEL_GC_OWNER=0: the sweeper threads take every list */
/* Claim and run tasks until the list is exhausted. Off the scheduler lock. */
static int sp_sweep_run_tasks(void) {
  int ran = 0;
  for (;;) {
    int i = SP_ATOMIC_FETCH_ADD(&g_sw_next, 1, __ATOMIC_RELAXED);
    if (i >= g_sw_ntasks) break;
    sp_sweep_task(&g_sw_tasks[i]);
    ran++;
  }
  return ran;
}
static unsigned       g_stw_epoch = 0;  /* bumped each collection; scopes g_nparked to one */
static SP_TLS int     g_collector_active = 0;  /* this worker is mid-collection (re-entrancy guard) */
static int            g_shutdown = 0;   /* set at drain so helper workers exit their loop */
static int            g_workers_started = 0;  /* helper pool + monitor spawned lazily on the first Thread */
static int            g_worker_cap = 0;       /* max helper workers: cores, or SPINEL_WORKERS - 1 (main is one of the N) */
static int            g_helpers_spawned = 0;  /* helpers created so far; ids 1..g_helpers_spawned */
static pthread_cond_t g_sched_work = PTHREAD_COND_INITIALIZER;   /* idle workers wait for runnable work */
static pthread_cond_t g_stw_request = PTHREAD_COND_INITIALIZER;  /* collector waits for parks */
static pthread_cond_t g_stw_release = PTHREAD_COND_INITIALIZER;  /* parked workers wait for clear */
static pthread_cond_t g_sysmon_cv = PTHREAD_COND_INITIALIZER;    /* the monitor thread waits here */
static pthread_t      g_sysmon;                                  /* monitor: wakes sleepers + preempts */
static int            g_sysmon_started = 0;
static int            g_sysmon_idle = 0; /* monitor parked on g_sysmon_cv: 1 with nothing to time (a thread that starts running must signal it), 2 in a timed wait (one that ends too late for a starting thread's timeslice is signalled too: g_sysmon_wake_at) */
static int            g_sysmon_pipe[2] = { -1, -1 };  /* self-pipe: wake the monitor out of poll() */
int sp_io_make_pipe(int fds[2]);   /* lib/sp_io.c: a close-on-exec pipe */
static void sp_sched_report_stats(void);   /* SPINEL_SCHED_STATS, below */
static void sched_make_runnable(sp_thread *t);   /* below */
/* What the monitor actually did, for SPINEL_SCHED_STATS=1. The cost of a wake
   is O(parked) three times over (rebuild, poll, unlink), so the number that
   sizes a deployment is iterations x set size -- and neither is visible from
   outside the process. Counted unconditionally: they are four increments on
   the monitor's own thread, under the lock it already holds. (#4317) */
static unsigned long long g_mon_iters = 0;    /* monitor loop turns */
static unsigned long long g_mon_polls = 0;    /* poll(2) calls it made */
static unsigned long long g_mon_pollfds = 0;  /* descriptors handed to those polls */
static unsigned long long g_mon_regs = 0;     /* I/O parks registered */
static unsigned long long g_mon_readied = 0;  /* waiters the poll found ready */
/* The counters are bumped with relaxed atomics: the report can read them
   from an exit hook while the monitor still runs (#6489). */
#define SP_STAT_ADD(c, n) SP_ATOMIC_FETCH_ADD(&(c), (unsigned long long)(n), __ATOMIC_RELAXED)
#define SP_STAT_GET(c) SP_ATOMIC_LOAD(&(c), __ATOMIC_RELAXED)
/* Wake the monitor whether it idles on the condvar or blocks in poll(). PRE: lock held. */
static void sp_sysmon_wake(void) {
  if (SP_ATOMIC_LOAD(&g_sysmon_idle, __ATOMIC_SEQ_CST)) pthread_cond_signal(&g_sysmon_cv);
  else if (g_sysmon_pipe[1] >= 0) { char c = 1; ssize_t r = write(g_sysmon_pipe[1], &c, 1); (void)r; }
}
/* When the monitor, now in a TIMED wait (its condvar or its poll), comes back
   on its own; 0 while it is not in one. The monitor stores it before it waits
   and reads the workers' slots after; a worker that starts a thread stores
   its slot and then reads this, all sequentially consistent: so the monitor
   sees the thread and does not wait, or the worker sees how long it waits and
   signals it if that is too late for the thread's timeslice
   (run_thread_once).

   The monitor used to cap every timed wait at a tick instead, so that a
   starting thread need not signal it: a server with connections parked in
   timed reads -- which always has a deadline -- kept it turning two hundred
   times a second, idle or not, each turn a condvar timeout and a
   scheduler-lock round trip. */
static uint64_t g_sysmon_wake_at_bits = 0;   /* a double; 0 = not in a timed wait */
static inline double sysmon_wake_at(void) { uint64_t b = SP_ATOMIC_LOAD(&g_sysmon_wake_at_bits, __ATOMIC_SEQ_CST); double d; memcpy(&d, &b, sizeof d); return d; }
static inline void sysmon_wake_at_set(double d) { uint64_t b; memcpy(&b, &d, sizeof b); SP_ATOMIC_STORE(&g_sysmon_wake_at_bits, b, __ATOMIC_SEQ_CST); }

/* (#8702) The timers. A thread has at most one, for the wait it is in, and it
   lives in the thread: its generation (tm_armed, 0 when none) is what a cancel
   and a firing contend for, with one atomic operation each, so neither takes
   a lock. A new timer is staged on the parker's worker, under that worker's
   own lock (g_tm_stage), and the monitor moves it into its heap, which is its
   alone. An entry whose generation is no longer armed -- cancelled, or
   replaced by a newer wait's -- is stale, and the monitor drops it when it
   reaches it. One lock used to guard all of this, and with many threads in
   timed waits it was the work: 128 pairs passing a turn through timed
   ConditionVariable waits spent 72% of their CPU spinning on it, to schedule
   a timer at each wait and cancel it at each wake. */
typedef struct { double deadline; sp_thread *thread; uint64_t gen; int kind; } sp_tm_ent;
typedef struct { sp_wlock wl; size_t len, cap; sp_tm_ent *ent; long ncancel; } __attribute__((aligned(64))) sp_tm_stage;
static sp_tm_stage g_tm_stage[SP_MAX_WORKERS];   /* a worker's newly armed timers, for the monitor */
/* The monitor's next wake time: the earliest deadline it knows of, 0 when
   none. A new timer lowers it (sp_nearest_lower), and the monitor publishes
   it again from its heap each turn (sp_timer_next). With the event backend,
   the monitor can skip rebuilding the I/O poll set until a deadline is due. */
static uint64_t g_nearest_bits = 0;   /* a double; 0 = nothing timed */
static inline double nearest_get(void) { uint64_t b = SP_ATOMIC_LOAD(&g_nearest_bits, __ATOMIC_SEQ_CST); double d; memcpy(&d, &b, sizeof d); return d; }
static inline void nearest_set(double d) { uint64_t b; memcpy(&b, &d, sizeof b); SP_ATOMIC_STORE(&g_nearest_bits, b, __ATOMIC_SEQ_CST); }
/* Lower g_nearest to d, which is positive: a positive double orders as its
   bits do. Answers whether it lowered it, in which case the monitor waits
   for a later deadline and must be woken (sp_sysmon_nudge). */
static int sp_nearest_lower(double d) {
  uint64_t b; memcpy(&b, &d, sizeof b);
  uint64_t cur = SP_ATOMIC_LOAD(&g_nearest_bits, __ATOMIC_SEQ_CST);
  while (cur == 0 || b < cur)
    if (SP_ATOMIC_CAS(&g_nearest_bits, &cur, b, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
  return 0;
}
/* Wake a monitor whose wait is longer than a deadline just added, with no
   timer or list lock held (it takes the scheduler lock). The parker stores
   g_nearest and then reads g_sysmon_idle; the monitor stores g_sysmon_idle and
   then reads g_nearest before it waits, all sequentially consistent, so one of
   the two sees the other: the monitor does not wait past the deadline, or the
   parker signals it under the lock its condvar uses, which it can take only
   once that wait has released it. A monitor in poll() gets its pipe. */
static void sp_sysmon_nudge(int need) {
  if (!need) return;
  if (SP_ATOMIC_LOAD(&g_sysmon_idle, __ATOMIC_SEQ_CST)) {
    int had = g_sched_held;
    if (!had) SCHED_LOCK();
    sp_sysmon_wake();
    if (!had) SCHED_UNLOCK();
  }
  else sp_sysmon_wake();
}

/* The monitor's min-heap of entries, by value. Staged timers go in together
   each turn: most waits run to their deadline, so this keeps the park path
   cheap and amortizes heap repair across the batch, like IO::Event::Timers.
   Stale entries are left in place until they reach the root or compaction
   makes rebuilding cheaper. An entry holds its thread, so the thread is not
   collected until the entry is gone (sp_sched_globals_mark). */
enum { SP_TIMER_SLEEP, SP_TIMER_IO, SP_TIMER_WAIT };
#define SP_TIMER_COMPACT_MINIMUM 128
#define SP_TIMER_HEAPIFY_INSERT_RATIO 2
static sp_tm_ent *g_timer_heap = NULL;
static size_t g_timer_heap_len = 0, g_timer_heap_cap = 0;
/* Cancels are counted on the canceller's worker (its staging line's
   ncancel), and the monitor counts the stale entries it drops: the
   difference is roughly what the heap holds stale, for compaction. One
   shared count was an atomic add on every wake of a timed wait. */
static long g_timer_dropped = 0;   /* the monitor's own */
static void sp_sched_timer_expire(const sp_tm_ent *e, double now);
static inline int tm_live(const sp_tm_ent *e) {
  return SP_ATOMIC_LOAD(&e->thread->tm_armed, __ATOMIC_ACQUIRE) == e->gen;
}
static void tm_dropped(long n) { g_timer_dropped += n; }   /* the monitor dropped n stale entries */
static long tm_cancelled(void) {   /* cancels whose entries are not dropped yet, roughly */
  long c = -g_timer_dropped;
  for (int w = 0; w < sp_active_workers && w < SP_MAX_WORKERS; w++) c += SP_ATOMIC_LOAD(&g_tm_stage[w].ncancel, __ATOMIC_RELAXED);
  return c > 0 ? c : 0;
}

static int sp_timer_reserve(sp_tm_ent **items, size_t *capacity, size_t needed) {
  if (*capacity >= needed) return 1;
  size_t nc = *capacity ? *capacity : 16;
  while (nc < needed) {
    if (nc > SIZE_MAX / 2) { nc = needed; break; }
    nc *= 2;
  }
  if (nc > SIZE_MAX / sizeof(sp_tm_ent)) return 0;
  sp_tm_ent *next = (sp_tm_ent *)realloc(*items, nc * sizeof(sp_tm_ent));
  if (!next) return 0;
  *items = next;
  *capacity = nc;
  return 1;
}

static void sp_timer_swap(size_t a, size_t b) {
  sp_tm_ent tmp = g_timer_heap[a];
  g_timer_heap[a] = g_timer_heap[b];
  g_timer_heap[b] = tmp;
}

static void sp_timer_bubble_up(size_t i) {
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (g_timer_heap[parent].deadline <= g_timer_heap[i].deadline) break;
    sp_timer_swap(i, parent);
    i = parent;
  }
}

static void sp_timer_bubble_down(size_t i) {
  for (;;) {
    size_t left = i * 2 + 1;
    if (left >= g_timer_heap_len) return;
    size_t right = left + 1;
    size_t child = right < g_timer_heap_len &&
                   g_timer_heap[right].deadline < g_timer_heap[left].deadline ? right : left;
    if (g_timer_heap[i].deadline <= g_timer_heap[child].deadline) return;
    sp_timer_swap(i, child);
    i = child;
  }
}

static void sp_timer_heapify(void) {
  if (g_timer_heap_len > 1) {
    for (size_t i = g_timer_heap_len / 2; i > 0; i--) sp_timer_bubble_down(i - 1);
  }
}

static void sp_timer_pop_root(void) {
  if (!g_timer_heap_len) return;
  g_timer_heap[0] = g_timer_heap[--g_timer_heap_len];
  if (g_timer_heap_len) sp_timer_bubble_down(0);
}

/* Arm `thread`'s timer, from the thread itself as it is about to wait.
   Answers the timer's generation, or 0 when it could not be staged; *nudge
   is set when the monitor must be woken for it (sp_sysmon_nudge, once every
   lock is dropped). Takes this worker's staging lock, which only the monitor
   also takes. */
static uint64_t sp_timer_schedule(sp_thread *thread, double deadline, int kind, int *nudge) {
  int w = sp_worker_id;
  if (w < 0 || w >= SP_MAX_WORKERS) w = 0;
  sp_tm_stage *s = &g_tm_stage[w];
  uint64_t gen = ++thread->tm_seq;
  wl_lock(&s->wl);
  if (!sp_timer_reserve(&s->ent, &s->cap, s->len + 1)) { wl_unlock(&s->wl); return 0; }
  s->ent[s->len] = (sp_tm_ent){ deadline, thread, gen, kind };
  /* armed before the monitor can take the entry, which it does under this lock */
  SP_ATOMIC_STORE(&thread->tm_armed, gen, __ATOMIC_RELEASE);
  /* sequentially consistent, as is the monitor's read of it after it
     publishes g_nearest (sp_timer_next): the monitor finds this entry, or
     the lowering below comes after what it published */
  SP_ATOMIC_STORE(&s->len, s->len + 1, __ATOMIC_SEQ_CST);
  wl_unlock(&s->wl);
  int lowered = sp_nearest_lower(deadline);
  if (nudge) *nudge = lowered;
  return gen;
}

/* Disarm `thread`'s timer, if it has one: its entry is stale from now on,
   and the monitor drops it when it reaches it. No lock: a firing takes the
   generation with a compare-and-swap, so of the two exactly one gets it. */
static void sp_timer_cancel(sp_thread *thread) {
  if (!SP_ATOMIC_LOAD(&thread->tm_armed, __ATOMIC_ACQUIRE)) return;
  if (SP_ATOMIC_EXCHANGE(&thread->tm_armed, 0, __ATOMIC_ACQ_REL)) {
    int w = sp_worker_id;
    if (w < 0 || w >= SP_MAX_WORKERS) w = 0;
    SP_ATOMIC_FETCH_ADD(&g_tm_stage[w].ncancel, 1, __ATOMIC_RELAXED);
  }
}

/* Move what the workers have staged into the heap, dropping what went stale
   meanwhile, and compact the heap once most of it is stale. PRE: the monitor. */
static void sp_timer_flush(void) {
  size_t old_len = g_timer_heap_len;
  int nw = sp_active_workers;
  for (int w = 0; w < nw && w < SP_MAX_WORKERS; w++) {
    sp_tm_stage *s = &g_tm_stage[w];
    if (!SP_ATOMIC_LOAD(&s->len, __ATOMIC_SEQ_CST)) continue;
    wl_lock(&s->wl);
    if (sp_timer_reserve(&g_timer_heap, &g_timer_heap_cap, g_timer_heap_len + s->len)) {
      long stale = 0;
      for (size_t i = 0; i < s->len; i++) {
        if (tm_live(&s->ent[i])) g_timer_heap[g_timer_heap_len++] = s->ent[i];
        else stale++;
      }
      SP_ATOMIC_STORE(&s->len, 0, __ATOMIC_SEQ_CST);
      if (stale) tm_dropped(stale);
    }
    wl_unlock(&s->wl);
  }
  long cancelled = tm_cancelled();
  if (cancelled >= SP_TIMER_COMPACT_MINIMUM && (size_t)cancelled * 2 > g_timer_heap_len) {
    size_t write = 0;
    for (size_t i = 0; i < g_timer_heap_len; i++)
      if (tm_live(&g_timer_heap[i])) g_timer_heap[write++] = g_timer_heap[i];
    tm_dropped((long)(g_timer_heap_len - write));
    g_timer_heap_len = write;
    sp_timer_heapify();
    return;
  }
  size_t added = g_timer_heap_len - old_len;
  if (!added) return;
  if (old_len == 0 || added > old_len * SP_TIMER_HEAPIFY_INSERT_RATIO) sp_timer_heapify();
  else for (size_t i = old_len; i < g_timer_heap_len; i++) sp_timer_bubble_up(i);
}

/* The earliest live entry, dropping the stale ones above it. PRE: the monitor. */
static sp_tm_ent *sp_timer_peek(void) {
  while (g_timer_heap_len && !tm_live(&g_timer_heap[0])) { sp_timer_pop_root(); tm_dropped(1); }
  return g_timer_heap_len ? &g_timer_heap[0] : NULL;
}

/* Fire what is due: each entry is taken off the heap and its generation
   claimed from its thread, then expired, which takes its list's lock. A
   generation a cancel took first is not fired, and one claimed here is not
   the generation of any later wait. PRE: the monitor. */
static void sp_timer_fire_due(double now) {
  sp_timer_flush();
  for (;;) {
    sp_tm_ent due[64]; int n = 0;
    sp_tm_ent *e;
    while (n < 64 && (e = sp_timer_peek()) && e->deadline <= now) {
      sp_tm_ent x = *e;
      sp_timer_pop_root();
      uint64_t g = x.gen;
      if (SP_ATOMIC_CAS(&x.thread->tm_armed, &g, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) due[n++] = x;
      else tm_dropped(1);
    }
    for (int i = 0; i < n; i++) sp_sched_timer_expire(&due[i], now);
    if (n < 64) return;
  }
}
/* The next deadline, published as g_nearest. A worker that stages a timer
   stores its staging length and then lowers g_nearest; this stores g_nearest
   and then reads the lengths, all sequentially consistent: so this finds the
   new timer, or the worker lowers what this published. With timers still
   being staged after a few rounds it answers `now`, so the monitor comes
   straight back for them. PRE: the monitor. */
static double sp_timer_next(void) {
  for (int round = 0; round < 4; round++) {
    sp_timer_flush();
    sp_tm_ent *e = sp_timer_peek();
    double d = e ? e->deadline : 0.0;
    nearest_set(d);
    int staged = 0, nw = sp_active_workers;
    for (int w = 0; w < nw && w < SP_MAX_WORKERS && !staged; w++)
      staged = SP_ATOMIC_LOAD(&g_tm_stage[w].len, __ATOMIC_SEQ_CST) != 0;
    if (!staged) return d;
  }
  double now = sp_monotonic_now();
  nearest_set(now);
  return now;
}

/* ---- preemption (design §5): timeslice tracking + the one safepoint flag ----
 * The monitor watches how long each worker has run its current green thread; past
 * a quantum it sets that thread's preempt_request, raises the safepoint flag, and
 * SIGURGs the worker. The thread yields at its next safepoint poll -- which the
 * codegen emits at loop back-edges, i.e. points that hold no runtime lock, so a
 * preempting thread always parks at a safe point. g_npreempt counts outstanding
 * requests so the flag (shared with GC stop-the-world) is cleared only when no
 * reason remains. All of this is touched solely under g_sched_lock. */
/* The timeslice is RUBY_THREAD_TIMESLICE milliseconds, as in CRuby, and 10
   unless that is set; the monitor checks it twice a slice, at most every 5 ms.
   Both are fixed at the first Thread.new (sp_sched_start_workers). */
static double g_quantum = 0.010, g_tick = 0.005;
#define SP_PREEMPT_QUANTUM g_quantum   /* a green thread runs this long before it must yield */
#define SP_PREEMPT_TICK    g_tick      /* monitor re-checks worker timeslices this often while busy */
/* (#8702) and keeps ticking this long after it last saw a worker busy, before
   it waits untimed: a thread that starts while it ticks needs no signal */
#define SP_MON_LINGER      (4 * SP_PREEMPT_TICK)
/* While a worker started a thread this recently, the monitor's timed waits
   stay within a quantum, so a run that starts then finds the monitor coming
   back in time on its own and does not signal it (run_thread_once): a server
   under load starts a thread far more often than this and never signals it;
   an isolated run (a once-a-second timer, an idle accept loop's poll) costs
   at most two short monitor turns besides its signal. 1.0 s kept a 0.5 s
   accept poll warm forever: a hundred monitor turns a second at idle. */
#define SP_SYSMON_WARM     (2 * SP_PREEMPT_QUANTUM)
static int g_npreempt = 0;         /* preempt_requests set but not yet consumed */
static int g_preempt_sig = SIGURG; /* the signal the monitor sends; SPINEL_PREEMPT_SIGNAL overrides */
/* SPINEL_WAKE_PREEMPT=1 (#8702): a woken thread goes to the front of its home
   worker's queue, and the thread running there yields to it at its next
   safepoint, rather than the woken one waiting out the timeslices ahead of
   it. Off by default: where wakes are frequent it costs the CPU-bound threads
   throughput. */
static int g_wake_preempt = 0;
typedef struct { pthread_t tid; sp_thread *cur; unsigned long long since_bits; int active;
                 /* A STARTED green thread is pinned to its home worker, so the wake
                    that readies it has exactly one worker to reach. Waiting on one
                    shared condvar meant that wake had to BROADCAST -- every idle
                    worker rose, serialised on the scheduler lock, found nothing and
                    slept again, with the monitor (the only thing that readies I/O
                    waiters) queued behind them. Adding workers then subtracted
                    throughput (#4305). Each helper waits on its own condvar instead;
                    `idle` says it is on it, and is written only under the lock, so a
                    wake that lands between the enqueue and the wait is not lost. */
                 pthread_cond_t cv; int idle;
                 /* (b), #4306: the worker waits on its OWN readiness set, holding the
                    descriptors of the threads pinned to it -- so a thread it can run
                    is one it wakes for itself, with no monitor round trip and no
                    condvar hand-off. `kick` is a self-pipe in that set: it is how a
                    stop-the-world, a shutdown, or work enqueued for this worker
                    breaks it out of the wait, since a signal on `cv` no longer
                    reaches it there. `ev_waiting` says which of the two it is on,
                    and like `idle` it is written only under the lock. */
                 int evfd; int kick[2]; int kick_armed; int ev_waiting;
                 /* Green threads whose home this worker is (pinned here for life, see
                    home_wid). A thread's first run fixes its home, so where an
                    unstarted thread is first run decides the balance for as long as
                    it lives -- for a server, one connection per thread, that is the
                    whole run. Counted so sched_pick can place a new thread on the
                    worker with the fewest. */
                 int pinned;
                 /* Parked for work in its idle wait (the readiness set with a
                    timeout, or the condvar), and whether a wake has been aimed at
                    it since it parked: an unpinned wake goes to a parked worker no
                    earlier wake has claimed (sched_wake_idle_helper). Atomics: a
                    worker that idles without the scheduler lock (sp_worker_unlocked)
                    writes them without it. */
                 int parked; int wake_sent;
                 /* (#8702) the I/O of the threads pinned here, under io_wl: the
                    threads parked on it, and their entries by descriptor */
                 sp_wlock io_wl; sp_thread *io_waiters; sp_ev_waiter **ev_tab; int ev_cap;
               } __attribute__((aligned(64))) sp_wslot;
static sp_wslot   g_wslot[SP_MAX_WORKERS];   /* per-worker: the green thread it runs + when it started */
/* The worker writes its slot's cur and since without the scheduler lock
   (#8702: it runs its own queue on the fast path), and the monitor reads
   them under it: both are atomics, since as its double's bits, written
   before cur is published and read after. */
static inline void wslot_set_since(int w, double t) {
  unsigned long long b; memcpy(&b, &t, sizeof b);
  SP_ATOMIC_STORE(&g_wslot[w].since_bits, b, __ATOMIC_RELAXED);
}
static inline double wslot_since(int w) {
  unsigned long long b = SP_ATOMIC_LOAD(&g_wslot[w].since_bits, __ATOMIC_RELAXED);
  double t; memcpy(&t, &b, sizeof t); return t;
}
static void sp_recompute_safepoint_flag(void) {   /* PRE: g_sched_lock held */
  SP_SAFEPOINT_SET(g_stw_active || g_npreempt > 0);
}
/* Ask r, running on worker w, to yield at its next safepoint poll: at the end
   of its timeslice (the monitor) or for a woken thread (SPINEL_WAKE_PREEMPT).
   A worker that stops running r clears cur without the scheduler lock, then
   looks for a request (run_thread_once); this sets the request, then reads cur
   again, all four sequentially consistent, so one of the two sees the other:
   the worker retires the request, or this takes it back. A request left on a
   thread that had stopped kept the safepoint flag up, every thread's polls on
   the scheduler lock, until that thread ran again. PRE: g_sched_lock held. */
static void sched_preempt(int w, sp_thread *r) {
  if (SP_ATOMIC_LOAD(&r->preempt_request, __ATOMIC_ACQUIRE)) return;
  SP_ATOMIC_STORE(&r->preempt_request, 1, __ATOMIC_SEQ_CST);
  if (SP_ATOMIC_LOAD(&g_wslot[w].cur, __ATOMIC_SEQ_CST) != r) {
    SP_ATOMIC_STORE(&r->preempt_request, 0, __ATOMIC_RELEASE);
    return;
  }
  g_npreempt++;
  sp_recompute_safepoint_flag();
  /* Nudge it to its next safepoint poll. The handler stores the flag just
     set under the lock, so the poll comes without the signal too. macOS
     does not get it: a signal that lands on a thread waiting for a
     pthread_rwlock's write lock (OpenSSL takes those) can lose the wakeup,
     and the thread waits forever (#9484). */
#ifndef __APPLE__
  pthread_kill(g_wslot[w].tid, g_preempt_sig);
#endif
}
/* The preemption signal lands on the target worker's own stack. Re-assert the
   flag so the worker sees it even if it was about to clear the lock-free read;
   the actual yield happens cooperatively at the next safepoint poll (kept minimal
   and async-signal-safe -- a lone relaxed atomic store). */
static void sp_preempt_handler(int sig) { (void)sig; SP_SAFEPOINT_SET(1); }
/* Wake one helper that can take unpinned work; main (worker 0, on g_sched_work)
   is the fallback when no helper is idle. */
/* Main waits on its condvar OR, once it has a readiness set, inside that --
   and a condvar signal does not reach it there. Every wake aimed at main goes
   through here, because the one that did not (quiescence) left it sitting out
   its 50ms backstop, which is a ping-pong at 8k hops a second instead of 190k
   (#4306). */
/* Wake one worker, whichever kind of wait it is on.
   The kick BYTE is durable and the ev_waiting FLAG is not: a worker sets the
   flag under the lock and enters its wait after releasing it, so a kick
   conditioned on the flag is lost in exactly that window -- the worker then
   sleeps its whole backstop. That is how a stop-the-world came to wait one
   out (the collector holds the lock and waits for every worker to park), and
   it is the wake that went missing about once in four hundred hops. A byte
   written before the wait is still there when it starts, because the kick
   descriptor is armed for the whole time the worker has a set. */
static void sched_kick_worker(int wid) {   /* PRE: sched lock held */
  if (wid < 0 || wid >= SP_MAX_WORKERS) return;
#ifdef SP_EV_BACKEND
  if (g_wslot[wid].evfd > 0) {
    /* ourselves: we loop and re-pick. Told by the OS thread, not by
       sp_worker_id: the monitor, which wakes threads whose timers are due,
       keeps the 0 it started with and read itself as main, so a wake for
       main from a timer never reached it (#9840, where main is the only
       worker) */
    if (!pthread_equal(g_wslot[wid].tid, pthread_self())) sp_ev_kick(wid);
    return;
  }
#endif
  if (wid == 0) { pthread_cond_broadcast(&g_sched_work); return; }
  RQ_LOCK(wid); if (g_wslot[wid].idle) pthread_cond_signal(&g_wslot[wid].cv); RQ_UNLOCK(wid);
}

/* (#8702) Main is about to wait in its pump, and no wake has reached it
   since: main raises this before it reads the counts to decide to wait, and
   every wake aimed at it lowers it. A helper going idle without the scheduler
   lock reads it after its own count changes, all sequentially consistent: so
   main sees those changes, or the helper sees it raised and takes the lock to
   wake main (sp_worker_idle_unlocked). Lowered, main is awake or about to be,
   and reads the counts again before it waits again. */
static int g_main_waits = 0;
static void sched_wake_main(void) {   /* PRE: sched lock held */
  SP_ATOMIC_STORE(&g_main_waits, 0, __ATOMIC_SEQ_CST);
#ifdef SP_EV_BACKEND
  if (g_wslot[0].evfd > 0) { sched_kick_worker(0); return; }
#endif
  pthread_cond_broadcast(&g_sched_work);
}

static void sched_wake_idle_helper(void) {   /* PRE: sched lock held */
  /* A helper parked for work that no earlier wake has claimed. Taking the
     first helper with a readiness set, as below, kicked worker 1 for every
     spawn whether it was idle or busy: eight threads spawned in a row were
     eight kicks to one worker while the other idle helpers slept, and the
     threads queued behind it. The wake-all on every join used to rouse the
     others and hid it. */
  /* A worker idling without the scheduler lock raises `parked` and then
     looks at the queues again (sp_worker_idle_unlocked); the thread this
     wake is for was queued before this, and the fence orders the two the
     same way here: so the worker finds the thread, or this finds it parked. */
  SP_ATOMIC_FENCE(__ATOMIC_SEQ_CST);
  for (int i = 1; i < sp_active_workers; i++)
    if (SP_ATOMIC_LOAD(&g_wslot[i].parked, __ATOMIC_SEQ_CST) && !SP_ATOMIC_LOAD(&g_wslot[i].wake_sent, __ATOMIC_RELAXED)) {
      SP_ATOMIC_STORE(&g_wslot[i].wake_sent, 1, __ATOMIC_RELAXED);
      sched_kick_worker(i);
      return;
    }
  for (int i = 1; i < sp_active_workers; i++) {
#ifdef SP_EV_BACKEND
    if (g_wslot[i].evfd > 0) { sched_kick_worker(i); return; }
#endif
    if (g_wslot[i].idle) { sched_kick_worker(i); return; }
  }
  sched_wake_main();
}
/* Every waiter re-checks state: helpers on their own condvars, main on its
   pump. Used where the state change is not one thread becoming runnable --
   shutdown, the STW barrier, quiescence. */
static unsigned char g_native_out[SP_MAX_WORKERS];    /* out of the world, roots published: SP_OUT_IDLE / SP_OUT_NATIVE, or 0 */
#define SP_OUT_IDLE   1   /* in the scheduler's idle wait: can be kicked to sweep its own lists */
#define SP_OUT_NATIVE 2   /* in a blocking native call: a sweeper thread takes its lists */
/* mode 0: everyone (shutdown). 1: a barrier being raised -- a worker out of
   the world is counted already and stays asleep. 2: a barrier released --
   the idle ones are kicked so each sweeps its own young lists now, while
   its cache is warm, rather than at its next wake or by a sweeper thread. */
static void sched_wake_all_workers(int mode) {   /* PRE: sched lock held */
  for (int i = 1; i < sp_active_workers; i++) {
    unsigned char out = SP_ATOMIC_LOAD(&g_native_out[i], __ATOMIC_RELAXED);
    if (mode == 1 && out) continue;
    if (mode == 2 && out != SP_OUT_IDLE) continue;
#ifdef SP_EV_BACKEND
    if (g_wslot[i].evfd > 0) { sched_kick_worker(i); continue; }
#endif
    sched_kick_worker(i);
  }
  { unsigned char out0 = SP_ATOMIC_LOAD(&g_native_out[0], __ATOMIC_RELAXED);
    if (!((mode == 1 && out0) || (mode == 2 && out0 != SP_OUT_IDLE))) sched_wake_main(); }
}
/* Wake the one worker that can run a thread pinned to `wid` (see home_wid).
   Main's worker (0) waits in its pump on the shared condvar, and a signal
   there could be taken by a helper instead, so it gets the broadcast. */
static void sched_wake_home(int wid) {   /* PRE: sched lock held */
  if (wid >= 0) { sched_kick_worker(wid); return; }
  sched_wake_idle_helper();   /* unpinned: any worker will do */
}
#define SCHED_WAKE()    sched_wake_idle_helper()
/* The main thread is the root fiber of worker 0, so making it runnable --
   a wake on a wait list (a thread's joiners among them), its I/O or timer,
   or a #wakeup -- has
   one worker to reach. Those wakes used to rouse every worker: from when
   all of them waited on one condvar and a signal could reach a helper
   instead of main. Each helper has its own wait since #4305/#4306, and
   none of them waits for anything a woken main or a dead thread changes,
   so the broadcast only woke them to find nothing -- for every join and
   every ConditionVariable signal to main, one kick per worker. */
#define SCHED_WAKE_MAIN() sched_wake_main()   /* main's pump alone */

/* Park the calling worker at the barrier until the collection finishes,
   publishing its running green thread's roots first. PRE: g_sched_lock held. */
/* A worker inside a blocking native call (an ffi_func declared `blocking:
   true`) has left the world: it runs no Ruby, touches no Ruby object, and
   its roots were published on the way in, so a collection raised while it
   is out counts it as parked and marks the fibers it recorded, and the
   barrier does not wait for the call to return. On the way back, a
   collection in progress is waited out. The count of such workers joins
   g_nparked in the collector's wait; a worker that returns while the
   barrier is up moves itself from one count to the other before waiting,
   so the collector's condition never goes backwards. */
static int       g_nnative = 0;                        /* workers out in a blocking native call */
static SP_TLS int g_native_depth = 0;                  /* this worker: nested enter/leave */
static sp_Fiber *g_native_fiber[SP_MAX_WORKERS][2];    /* per worker: the fibers to mark while out */
static int       g_native_nfiber[SP_MAX_WORKERS];
static void sp_stw_publish_locked(void);
static void sp_stw_park_locked(void);
static int sp_cs_chunks_settled(int wid);   /* the concurrent sweep is out of this slot's chunks (below) */
static SP_TLS int g_native_noop = 0;                   /* entered before the pool existed: nothing to undo */
/* Leave the world (PRE: sched lock held, no collection active): publish, record
   the fibers to mark, and count this worker as out. Shared by a blocking
   native call and by the idle wait in the scheduler loop -- an idle worker
   has nothing running either, and waking thirty of them so each could park
   was most of what the barrier waited for. */
static void sp_sched_signal_if_quiescent(void);   /* below */
static void sp_out_enter_locked(int wid, int how) {
  sp_stw_publish_locked();
  if (wid >= 0 && wid < SP_MAX_WORKERS) {
    sp_Fiber *root = sp_fiber_worker_root();
    g_native_nfiber[wid] = 0;
    if (sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = sp_fiber_current;
    if (root && root != sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = root;
    SP_ATOMIC_STORE(&g_native_out[wid], (unsigned char)how, __ATOMIC_RELEASE);
  }
  int nout = SP_ATOMIC_FETCH_ADD(&g_nnative, 1, __ATOMIC_SEQ_CST) + 1;
  if (g_stw_active && g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) >= sp_active_workers - 1)
    pthread_cond_signal(&g_stw_request);
  /* (#8702) The last helper to go idle wakes a main that waits for nothing
     to be left -- the exit drain, or a join that can no longer finish -- if
     nothing is (counts_quiet), under the lock that main looks and waits
     under. Nothing left to run means every helper goes idle, so the last one
     to go sees it. This was a look at the counts after every thread that
     stopped, which is a pass over every worker's slot now. */
  if (how == SP_OUT_IDLE && wid > 0 && nout >= sp_active_workers - 1) sp_sched_signal_if_quiescent();
}
/* Back in the world (PRE: sched lock held). A collection in progress counted
   this worker as out; it becomes a parker of this epoch and waits for the
   release like one (the fibers it recorded stay valid until then: nothing
   has run on it). */
static void sp_out_leave_locked(int wid) {
  if (g_stw_active) {
    SP_ATOMIC_FETCH_SUB(&g_nnative, 1, __ATOMIC_SEQ_CST);
    unsigned my_epoch = g_stw_epoch;
    g_nparked++;
    if (g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) >= sp_active_workers - 1) pthread_cond_signal(&g_stw_request);
    while (g_stw_active && g_stw_epoch == my_epoch) {
      if (g_cs_help && SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) {
        SCHED_UNLOCK(); sp_cs_help_run(); SCHED_LOCK(); continue;
      }
      pthread_cond_wait(&g_stw_release, &g_sched_lock);
    }
    if (g_stw_epoch == my_epoch) g_nparked--;
  }
  else SP_ATOMIC_FETCH_SUB(&g_nnative, 1, __ATOMIC_SEQ_CST);
  if (wid >= 0 && wid < SP_MAX_WORKERS) { SP_ATOMIC_STORE(&g_native_out[wid], 0, __ATOMIC_RELEASE); g_native_nfiber[wid] = 0; }
  /* the collection that ran while this worker was out left it its own young
     lists to sweep, and after a full cycle its slab chunks to release */
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0 || (g_cs_full && sp_slab_on > 0)) {
    SCHED_UNLOCK();
    if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sp_cs_owner_run(sp_worker_id);
    if (g_cs_full && sp_slab_on > 0 && sp_cs_chunks_settled(sp_worker_id)) sp_slab_release_worker(sp_worker_id);
    SCHED_LOCK();
  }
}
void sp_native_enter(void) {
  if (g_native_depth++ > 0) return;
  if (!g_workers_started) { g_native_noop = 1; return; }   /* one worker, no collector to leave the world for */
  SCHED_LOCK();
  /* a collection already running: park through it first, as a poll would */
  if (g_stw_active) sp_stw_park_locked();
  sp_out_enter_locked(sp_worker_id, SP_OUT_NATIVE);
  SCHED_UNLOCK();
}
void sp_native_leave(void) {
  if (--g_native_depth > 0) return;
  if (g_native_noop) { g_native_noop = 0; return; }
  SCHED_LOCK();
  sp_out_leave_locked(sp_worker_id);
  SCHED_UNLOCK();
}
/* An ffi_callback from inside a blocking call runs Ruby, so it comes back into
   the world for the method and goes out again when the method has returned:
   by then its frames are off the shadow stack, and what the way out publishes
   is what stands below the call. Only the trampoline can do this: a Ruby method
   that went out itself would publish its own frame and return. The depth drops
   to 0 meanwhile, as this worker is in the world like any other, so a raise
   that unwinds out of the blocking call lands in that state, and a blocking
   call the method makes starts over. Answers what the way out restores. */
int sp_native_callback_enter(void) {
  int depth = g_native_depth;
  if (depth == 0) return 0;   /* a plain call: this worker never left */
  g_native_depth = 0;
  if (g_native_noop) g_native_noop = 0;   /* entered before the pool existed: it never left either */
  else {
    SCHED_LOCK();
    sp_out_leave_locked(sp_worker_id);
    SCHED_UNLOCK();
  }
  return depth;
}
/* A real way out even if the call began with no pool: the method may have
   started the first thread, and its collections do not wait for this call. */
void sp_native_callback_leave(int depth) {
  if (depth == 0) return;
  sp_native_enter();
  g_native_depth = depth;
}
/* What a parking worker publishes for the collector: the shadow-stack roots
   plus this worker's live match registers (TLS, so the collector's globals
   hook does not reach them) into the green thread's saved snapshot, then
   the worker's own root depth is restored -- the snapshot keeps them. The
   pointer-keyed string length cache goes too: the collection about to run
   may recycle a string's address. */
static void sp_stw_publish_locked(void) {
  int saved_nroots = sp_gc_nroots;
  sp_re_push_match_roots();
  if (sp_safepoint_publish_hook) sp_safepoint_publish_hook();   /* TU in-flight exc / proc homes */
  sp_fiber_publish_current_roots();
  sp_gc_nroots = saved_nroots;
  sp_str_lcache_clear();
}
static void sp_stw_park_locked(void) {
  /* Publish the shadow-stack roots plus this worker's live match registers (TLS,
     so the collector's globals hook does not reach them) into the green thread's
     saved snapshot, then restore our own root depth -- the snapshot keeps them. */
  sp_stw_publish_locked();
  /* Record the fibers the collector must mark for this worker: the green thread
     it is running (sp_fiber_current) AND its root fiber. The root fiber holds the
     worker's own suspended context -- for the main thread that is the top-level
     locals, saved when it transferred into the green thread it is pumping -- and
     it is not on the global fiber list, so without this it would be missed. */
  sp_Fiber *root = sp_fiber_worker_root();
  if (sp_fiber_current && g_n_parked_fiber < 2 * SP_MAX_WORKERS)
    g_parked_fiber[g_n_parked_fiber++] = sp_fiber_current;   /* collector marks these */
  if (root && root != sp_fiber_current && g_n_parked_fiber < 2 * SP_MAX_WORKERS)
    g_parked_fiber[g_n_parked_fiber++] = root;
  /* Park for the current collection (epoch). g_nparked counts only this epoch's
     parkers: when the next collection starts it bumps the epoch and resets the
     count, and a straggler from a finished collection (epoch mismatch) must not
     touch the new count -- otherwise the new collector could see a stale count
     and proceed before anyone has actually parked, marking an incomplete root
     set (then sweeping a still-live root). */
  unsigned my_epoch = g_stw_epoch;
  g_nparked++;
  if (g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) >= sp_active_workers - 1) pthread_cond_signal(&g_stw_request);
  while (g_stw_active && g_stw_epoch == my_epoch) {
    /* Sweep our own slot if the collector has asked for it. Dropping the
       scheduler lock is safe and necessary: nothing else touches this slot
       (the collector claims only its own), the mutators are all parked here,
       and holding the lock through a free-heavy walk would serialize exactly
       what this phase exists to parallelize. */
    if (g_sweep_go && SP_ATOMIC_LOAD(&g_sw_next, __ATOMIC_RELAXED) < g_sw_ntasks) {
      SCHED_UNLOCK();
      int ran = sp_sweep_run_tasks();
      SCHED_LOCK();
      g_sw_done += ran;
      pthread_cond_signal(&g_sweep_cv);
      continue;
    }
    /* The collector's drain wants helpers: take a seat if one is left, run
       the drain to its end, and come back here. */
    if (g_mk_go && g_mk_seen != g_mk_gen) {
      g_mk_seen = g_mk_gen;
      if (g_mk_joined < g_mk_max) {
        g_mk_joined++; g_mk_active++;
        SCHED_UNLOCK();
        sp_gc_mark_par_run();
        SCHED_LOCK();
        if (--g_mk_active == 0) pthread_cond_broadcast(&g_mk_cv);
      }
      continue;
    }
    /* The previous concurrent sweep is not done and the collector asked for
       hands: what the sweeper threads have not claimed yet is claimed here,
       under the barrier, by everyone who is parked. */
    if (g_cs_help && SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) {
      SCHED_UNLOCK();
      sp_cs_help_run();
      SCHED_LOCK();
      continue;
    }
    pthread_cond_wait(&g_stw_release, &g_sched_lock);
  }
  if (g_stw_epoch == my_epoch) g_nparked--;
  /* Released: this worker's own young lists from the collection that just
     ended are swept HERE, by their owner, before it runs any program. The
     slots it frees are the ones it allocates from next, still in its own
     cache from the walk; swept by another core they came back cold, and the
     mutators measured slower than under the stop-the-world sweep. */
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0 || (g_cs_full && sp_slab_on > 0)) {
    SCHED_UNLOCK();
    if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sp_cs_owner_run(sp_worker_id);
    /* and, after a full cycle, its own slab chunks: the release that used
       to run for every worker under the next barrier */
    if (g_cs_full && sp_slab_on > 0 && sp_cs_chunks_settled(sp_worker_id)) sp_slab_release_worker(sp_worker_id);
    SCHED_LOCK();
  }
}
#else
#define SCHED_LOCK()    ((void)0)
#define SCHED_UNLOCK()  ((void)0)
#define RQ_LOCK(w)      ((void)(w))
#define RQ_UNLOCK(w)    ((void)(w))
#define REG_LOCK()      ((void)0)
#define REG_UNLOCK()    ((void)0)
static SP_TLS int g_sched_held = 0;
static inline void wl_lock(sp_wlock *l) { (void)l; }
static inline void wl_unlock(sp_wlock *l) { (void)l; }
#define SCHED_WAKE()    ((void)0)
#define SCHED_WAKE_MAIN() ((void)0)
#define sched_wake_home(w) ((void)(w))
#endif

/* A thread's state changes only under the scheduler lock, but some readers
   look at it without the lock: a contended Mutex spins while its owner is
   RUNNING (sp_mutex_spin_acquire), and #alive?, #status and #inspect answer
   from it. So every store is atomic, and those reads load it atomically; a
   plain store is a data race against them (ThreadSanitizer:
   sp_mutex_spin_acquire against sp_sched_block and runq_push). Relaxed is
   enough for the spin, which takes the value as a hint and lets the mutex's
   own exchange decide. The Thread readers load it with acquire: once they
   see DEAD they read has_exc and the exception beside it, and the exit path
   stores DEAD with release after those, so a thread that died of an
   exception is never reported as one that finished normally. */
#define SP_TH_SET_STATE(t, s) SP_ATOMIC_STORE(&(t)->state, (s), __ATOMIC_RELAXED)
/* (#8702) the park word (sp_thread.park). */
#define SP_PK_RUN     0   /* on a CPU */
#define SP_PK_PARKING 1   /* on a waiter list, still switching out */
#define SP_PK_PARKED  2   /* off its stack */
#define SP_PK_WOKEN   3   /* woken while PARKING: its worker queues it */
#define SP_TH_STATE(t)        SP_ATOMIC_LOAD(&(t)->state, __ATOMIC_ACQUIRE)

#ifdef SP_THREADS
static void sp_safepoint_preempt(void);   /* defined after the scheduler state below */
static void sp_sched_pass(void);   /* main's Thread.pass sweep, which the preemption at SPINEL_WORKERS=1 runs */
#endif

/* Safepoint poll body: codegen emits `if (sp_safepoint_flag) sp_safepoint();` at
   loop back-edges. Park if a stop-the-world is in progress, then yield if the
   monitor flagged this green thread as over its timeslice (design §5). */
void sp_safepoint(void) {
#ifdef SP_THREADS
  SCHED_LOCK();
  if (g_stw_active) sp_stw_park_locked();
  sp_safepoint_preempt();
  SCHED_UNLOCK();
#endif
}

/* Stop the world and collect (design 6.2). Called by sp_gc_alloc once over the
   threshold, with the heap lock released. Either become the collector -- set the
   barrier, wait for every other worker to park, then mark+sweep with exclusive
   heap access -- or, if a collection is already running, just park through it.
   At N=1 there are no other workers, so the wait is a no-op and this is exactly
   today's inline collect, routed through the barrier. */
static void sp_stw_collect_impl(int force);

void sp_stw_collect(void) { sp_stw_collect_impl(0); }

/* An EXPLICIT collection (GC.start / GC.compact). It must take the same barrier
   as a threshold-triggered one: the parallel sweep hands one slot to each other
   worker and waits for them, which only happens if they are parked here. Run
   straight from a mutator, the collector waited on sweeps nobody would do and
   any threaded program calling GC.start hung (#3781). Forced: an explicit
   request collects even when neither heap is over its trigger. */
void sp_gc_collect_request(void) {
#ifdef SP_THREADS
  sp_stw_collect_impl(1);
#else
  if (sp_gc_collect_fn) sp_gc_collect_fn();   /* (--gc=none: nothing to collect) */
#endif
  /* GC.start is a call of the program's, a safe point: what it freed is
     finalized before it returns, after the barrier has lifted */
  sp_fin_run_pending();
}

static void sp_stw_collect_impl(int force) {
#ifdef SP_THREADS
  /* Re-entrancy guard: a finalizer run during the sweep may allocate and cross
     the threshold again. We are already the collector with the world stopped
     (exclusive heap access), so just let that allocation proceed -- re-entering
     the barrier here would park the collector waiting on itself (deadlock). */
  if (g_collector_active) return;
#ifdef SP_CEXT
  /* Take CXL before requesting STW: a C loop need not poll, and an
     allocation made by its owner may recursively become the collector. */
  sp_cext_lock();
#endif
  SCHED_LOCK();
  if (g_stw_active) { sp_stw_park_locked(); SCHED_UNLOCK();
#ifdef SP_CEXT
    sp_cext_unlock();
#endif
    return; }
  if (!force && !sp_gc_collection_wanted()) { SCHED_UNLOCK();
#ifdef SP_CEXT
    sp_cext_unlock();
#endif
    return; }  /* another worker just collected */
  SP_ATOMIC_STORE(&g_stw_active, 1, __ATOMIC_SEQ_CST);   /* a worker's fast path reads it without the lock */
  g_stw_epoch++;     /* new epoch; a previous collection's stragglers won't be counted */
  g_nparked = 0;     /* this collection's park count starts fresh */
  SP_SAFEPOINT_SET(1);
  double bt0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  /* SPINEL_GC_PHASES: how many of the workers this barrier waits for are in
     their own sweep of the previous cycle's lists (a sweep does not look at
     the safepoint), against how many are running the program */
  int ph_sweeping = sp_gc_ph_on ? SP_ATOMIC_LOAD(&g_cs_running, __ATOMIC_RELAXED) : 0;
  /* wake idle workers (and main waiting in its pump) so they park at the barrier
     rather than sit through the collection without publishing their roots. */
  sched_wake_all_workers(1);   /* reaches an ev_waiting main and workers too, except those out */
  while (g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) < sp_active_workers - 1) pthread_cond_wait(&g_stw_request, &g_sched_lock);
  /* the workers out in a blocking native call: their roots were published
     on the way out, and the fibers they recorded are marked like a parked
     worker's (a worker that came back meanwhile is a parker of this epoch,
     still with its slot recorded, since nothing has run on it) */
  for (int w = 0; w < SP_MAX_WORKERS && w < sp_active_workers; w++) {
    if (!SP_ATOMIC_LOAD(&g_native_out[w], __ATOMIC_RELAXED)) continue;
    for (int f = 0; f < g_native_nfiber[w] && g_n_parked_fiber < 2 * SP_MAX_WORKERS; f++)
      g_parked_fiber[g_n_parked_fiber++] = g_native_fiber[w][f];
  }
  if (sp_gc_ph_on) { double pw = sp_monotonic_now() - bt0; sp_gc_ph_park += pw;
                     if (ph_sweeping > 0) { sp_gc_ph_park_sweeping += pw; sp_gc_ph_park_sweeping_n++; } }
  /* Our own root fiber holds this worker's suspended context (the main thread's
     top-level locals if it triggered the collection while pumping a green
     thread). We do not park, so record it here for the mark like a parked worker
     does -- our current fiber's roots are reached directly from our TLS stack. */
  {
    sp_Fiber *croot = sp_fiber_worker_root();
    if (croot && croot != sp_fiber_current && g_n_parked_fiber < 2 * SP_MAX_WORKERS)
      g_parked_fiber[g_n_parked_fiber++] = croot;
  }
  SCHED_UNLOCK();
  /* exclusive: every other worker is parked at a safepoint with roots published */
  g_collector_active = 1;
  sp_gc_collect_retune_all();   /* sweeps both heaps; marks parked fibers via sp_sched_globals_mark */
  /* an explicit GC.start answers once everything unreachable is gone: the
     concurrent sweep it started is finished here, still under the barrier */
  if (force && sp_gc_conc_wait_hook) sp_gc_conc_wait_hook();
  g_collector_active = 0;
  SCHED_LOCK();
  g_n_parked_fiber = 0;
  SP_ATOMIC_STORE(&g_stw_active, 0, __ATOMIC_SEQ_CST);
  sp_recompute_safepoint_flag();   /* keep the flag set if a preempt is still pending */
  pthread_cond_broadcast(&g_stw_release);
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sched_wake_all_workers(2);   /* the idle owners sweep their own lists */
  if (sp_gc_ph_on) sp_gc_ph_barrier += sp_monotonic_now() - bt0;
  SCHED_UNLOCK();
  /* the collector's own young lists, like every released worker's */
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sp_cs_owner_run(sp_worker_id);
  if (g_cs_full && sp_slab_on > 0 && sp_cs_chunks_settled(sp_worker_id)) sp_slab_release_worker(sp_worker_id);
#ifdef SP_CEXT
  sp_cext_unlock();
#endif
#else
  (void)force;
  sp_gc_collect_retune();
#endif
}

/* ---- scheduler state (single OS worker, so plain globals) ---- */
/* The main thread is a STATIC, not a GC allocation -- but `Thread.current`
   hands it to user code and to the runtime's own thread ops, and both the mark
   and the write barrier decide what a pointer is from the byte in FRONT of it.
   Without a guard that read is one byte before a global (ASAN reports it), and
   the barrier goes further: it fabricates a header there and writes a dirty bit
   into whatever .bss happens to precede this one. Lay the same 0xfd skip byte
   the root fiber uses, exactly one alignment unit wide so no padding can slip
   in between. */
static struct { char guard[_Alignof(sp_thread)]; sp_thread t; } g_main_thread_box
    = { .guard = { [_Alignof(sp_thread) - 1] = (char)SP_STATIC_GUARD }, .t = {0} };
#define g_main_thread (g_main_thread_box.t)   /* the main thread: runs on root, fiber == NULL */
static SP_TLS sp_thread *g_current = NULL;   /* per-worker: the green thread this worker runs now */
/* Run queues (design 3.1). A worker requeues a thread it just ran onto its OWN
   local queue (g_lrq[wid]) so a yielding thread reruns on the same worker (warm
   cache); spawns and wakeups, which have no worker affinity, land on the shared
   global queue (g_grq). A worker picks local-first, then global, then steals one
   from another worker before parking. All queues are guarded by g_sched_lock --
   this gives locality and load balancing without a second lock; reducing the
   lock itself would mean reworking the off-cpu handshake (deferred, see git log).
   The runnable count (RUNNABLE_INC) is the total parked-runnable count across
   every queue, for the quiescence/deadlock predicate. */
SP_TLS int sp_worker_id = 0;       /* exported: sp_alloc.h indexes the per-worker string heaps */       /* this worker's run-queue slot (0 = main); both builds */
/* a line each, as each worker's queue is now pushed and popped without
   the scheduler lock, concurrently with its neighbours'. */
typedef struct { sp_thread *head, *tail;
                 sp_thread *woke;   /* the last of the woken threads at its front (SPINEL_WAKE_PREEMPT) */
               } __attribute__((aligned(64))) sp_runq;
static sp_runq    g_grq;                 /* global run queue: spawned + woken threads */
static sp_runq    g_lrq[SP_MAX_WORKERS]; /* per-worker local run queues (rerun locality) */
/* The runnable count (threads sitting in any run queue right now) and the
   running count (workers executing a green thread): main's pump decides that nothing can
   wake it when both are zero. Every move raises the count it goes to before
   lowering the one it leaves, so a reader that sees both at one instant
   always counts a thread on its way between them. */
#ifdef SP_THREADS
/* (#8702) Kept per actor, not in one shared word. A wake, a pop, a run and
   a stop each changed the shared word, from every worker at once, and once
   the workers ran their own queues without the scheduler lock that word was
   where their time went: 95% of the samples of a push and a pop sat on its
   one atomic add. Each worker, the monitor, and one locked slot for any
   other thread now count what they do in a line of their own, which only
   they write: a sequence number, odd while the counts change, as a seqlock.
   A thread queued by one worker and run by another is counted up in the
   first's slot and down in the second's, so one slot can go below zero; the
   sums are what mean anything. Readers that need them exact read every slot
   twice, and take the sums only when no slot changed in between
   (counts_quiet): the slots were then all as read at one instant between
   the passes. The counts are words: on a 32-bit target a long long's
   atomic load and store are calls into libatomic, which the runtime does
   not link. */
typedef struct { unsigned seq; intptr_t runnable, running, timed, nio; } __attribute__((aligned(64))) sp_cnt_slot;
#define SP_CNT_SLOTS (SP_MAX_WORKERS + 2)
#define SP_CNT_MON   SP_MAX_WORKERS          /* the monitor's */
#define SP_CNT_OTHER (SP_MAX_WORKERS + 1)    /* any other thread's, under g_cnt_other_wl */
static sp_cnt_slot g_cnt[SP_CNT_SLOTS];
static SP_TLS sp_cnt_slot *g_cnt_me = NULL;   /* this thread's slot, if it has one */
/* Workers' slots in use: [0, g_cnt_hi). Raised by a worker before its first
   change of its slot, and read by a reader before and after its passes, so a
   worker it does not read did not change its slot before the reader's
   second pass. Read with the monitor's slot and the shared one: a pass over
   every possible worker's slot cost a join round a few microseconds. */
static int g_cnt_hi = 1;
static void cnt_claim(int wid) {   /* a worker, before it counts anything */
  int hi = SP_ATOMIC_LOAD(&g_cnt_hi, __ATOMIC_SEQ_CST);
  while (hi < wid + 1 && !SP_ATOMIC_CAS(&g_cnt_hi, &hi, wid + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
  g_cnt_me = &g_cnt[wid];
}
/* the i-th slot a reader reads: the workers' in use, then the monitor's and the shared one */
static inline sp_cnt_slot *cnt_slot(int i, int hi) { return i < hi ? &g_cnt[i] : &g_cnt[SP_CNT_MON + (i - hi)]; }
static sp_wlock g_cnt_other_wl = 0;
static void cnt_add4(long long drunnable, long long drunning, long long dtimed, long long dnio) {
  sp_cnt_slot *s = g_cnt_me;
  if (!s) { wl_lock(&g_cnt_other_wl); s = &g_cnt[SP_CNT_OTHER]; }
  unsigned q = SP_ATOMIC_LOAD(&s->seq, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&s->seq, q + 1, __ATOMIC_RELAXED);
  SP_ATOMIC_FENCE(__ATOMIC_RELEASE);   /* odd before the counts change */
  SP_ATOMIC_STORE(&s->runnable, SP_ATOMIC_LOAD(&s->runnable, __ATOMIC_RELAXED) + drunnable, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&s->running, SP_ATOMIC_LOAD(&s->running, __ATOMIC_RELAXED) + drunning, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&s->timed, SP_ATOMIC_LOAD(&s->timed, __ATOMIC_RELAXED) + dtimed, __ATOMIC_RELAXED);
  SP_ATOMIC_STORE(&s->nio, SP_ATOMIC_LOAD(&s->nio, __ATOMIC_RELAXED) + dnio, __ATOMIC_RELAXED);
  /* sequentially consistent, as the shared counts' atomic adds were: what a
     thread does after counting -- leave a list, make a wait visible -- is
     seen after the count by a reader that reads both */
  SP_ATOMIC_STORE(&s->seq, q + 2, __ATOMIC_SEQ_CST);
  if (s == &g_cnt[SP_CNT_OTHER]) wl_unlock(&g_cnt_other_wl);
}
#define RUNNABLE_INC() cnt_add4(1, 0, 0, 0)
#define RUNNABLE_DEC() cnt_add4(-1, 0, 0, 0)
#define RUNNING_INC()  cnt_add4(0, 1, 0, 0)
#define RUNNING_DEC()  cnt_add4(0, -1, 0, 0)
#define RUNNABLE_TO_RUNNING() cnt_add4(-1, 1, 0, 0)   /* one change: the thread is in one count or the other */
/* the two wait counts, kept the same way: threads in a sleep or a timed wait,
   and threads parked on I/O in a worker's set */
#define NTIMED_INC()   cnt_add4(0, 0, 1, 0)
#define NTIMED_DEC()   cnt_add4(0, 0, -1, 0)
#define NIO_INC()      cnt_add4(0, 0, 0, 1)
#define NIO_DEC()      cnt_add4(0, 0, 0, -1)
/* One pass over the slots: the sums, and each slot's sequence number.
   Answers 0 when a slot was changing. */
static int cnt_pass(int hi, unsigned *seq, long long *sum) {
  long long a = 0, b = 0, c_ = 0, d = 0;
  for (int i = 0; i < hi + 2; i++) {
    sp_cnt_slot *c = cnt_slot(i, hi);
    unsigned q = SP_ATOMIC_LOAD(&c->seq, __ATOMIC_SEQ_CST);
    if (q & 1) return 0;
    if (seq) seq[i] = q;
    a += SP_ATOMIC_LOAD(&c->runnable, __ATOMIC_RELAXED);
    b += SP_ATOMIC_LOAD(&c->running, __ATOMIC_RELAXED);
    c_ += SP_ATOMIC_LOAD(&c->timed, __ATOMIC_RELAXED);
    d += SP_ATOMIC_LOAD(&c->nio, __ATOMIC_RELAXED);
  }
  SP_ATOMIC_FENCE(__ATOMIC_ACQUIRE);
  sum[0] = a; sum[1] = b; sum[2] = c_; sum[3] = d;
  return 1;
}
/* The four sums -- runnable, running, timed, nio -- as of one instant: two
   passes, and the sums of the first are taken when no slot's sequence number
   moved by the second. With slots still changing after a few tries it
   answers 0: a thread was being queued, run, stopped or parked during the
   read, and the caller takes that as work (counts_quiet). */
static int counts_snapshot(long long *sum) {
  for (int tries = 0; tries < 4; tries++) {
    unsigned seq[SP_CNT_SLOTS];
    int hi = SP_ATOMIC_LOAD(&g_cnt_hi, __ATOMIC_SEQ_CST);
    if (!cnt_pass(hi, seq, sum)) continue;
    int same = 1;
    for (int i = 0; i < hi + 2 && same; i++)
      same = SP_ATOMIC_LOAD(&cnt_slot(i, hi)->seq, __ATOMIC_SEQ_CST) == seq[i];
    if (same && SP_ATOMIC_LOAD(&g_cnt_hi, __ATOMIC_SEQ_CST) == hi) return 1;
  }
  return 0;
}
/* Nothing runnable and nothing running, as of one instant. Counts that kept
   changing while it read them say no: the actor that makes them zero last
   does not see that (counts_quiet is read by the last helper to go idle,
   sp_out_enter_locked). */
static int counts_quiet(void) {
  long long sum[4];
  return counts_snapshot(sum) && sum[0] == 0 && sum[1] == 0;
}
/* The sums from one pass, for what only needs them roughly (a Thread.pass
   sweep's length, the helper pool's growth, the stats). */
static void counts_approx(int *runnable, int *running) {
  long long a = 0, b = 0;
  int hi = SP_ATOMIC_LOAD(&g_cnt_hi, __ATOMIC_ACQUIRE);
  for (int i = 0; i < hi + 2; i++) {
    a += SP_ATOMIC_LOAD(&cnt_slot(i, hi)->runnable, __ATOMIC_RELAXED);
    b += SP_ATOMIC_LOAD(&cnt_slot(i, hi)->running, __ATOMIC_RELAXED);
  }
  *runnable = a > 0 ? (int)a : 0; *running = b > 0 ? (int)b : 0;
}
/* Threads parked on I/O in a worker's set, from one pass: for the monitor's
   and Thread.pass's "is any thread waiting on I/O" */
static int nio_approx(void) {
  long long d = 0;
  int hi = SP_ATOMIC_LOAD(&g_cnt_hi, __ATOMIC_ACQUIRE);
  for (int i = 0; i < hi + 2; i++) d += SP_ATOMIC_LOAD(&cnt_slot(i, hi)->nio, __ATOMIC_RELAXED);
  return d > 0 ? (int)d : 0;
}
#else
/* One worker: the counts are its own. */
static uint64_t   g_counts = 0;
#define SP_COUNT_RUNNING_ONE ((uint64_t)1 << 32)
#define RUNNABLE_INC() ((void)(g_counts += 1))
#define RUNNABLE_DEC() ((void)(g_counts -= 1))
#define RUNNING_INC()  ((void)(g_counts += SP_COUNT_RUNNING_ONE))
#define RUNNING_DEC()  ((void)(g_counts -= SP_COUNT_RUNNING_ONE))
#define RUNNABLE_TO_RUNNING() ((void)(g_counts += SP_COUNT_RUNNING_ONE - 1))
static int counts_quiet(void) { return g_counts == 0; }
static int g_ntimed_n = 0, g_nio_n = 0;
#define NTIMED_INC() ((void)g_ntimed_n++)
#define NTIMED_DEC() ((void)g_ntimed_n--)
#define NIO_INC()    ((void)g_nio_n++)
#define NIO_DEC()    ((void)g_nio_n--)
static int counts_snapshot(long long *sum) { sum[0] = (uint32_t)g_counts; sum[1] = (long long)(g_counts >> 32); sum[2] = g_ntimed_n; sum[3] = g_nio_n; return 1; }
static int nio_approx(void) { return g_nio_n; }
static void counts_approx(int *runnable, int *running) { *runnable = (int)(uint32_t)g_counts; *running = (int)(g_counts >> 32); }
#endif
/* Threads in a sleep or a timed wait (Kernel#sleep, a timed ConditionVariable,
   Queue, join or I/O wait), for main's pump: such a thread wakes by itself in
   the end, so a wait for it is not a deadlock. Raised before the thread parks
   and lowered only once it runs again, so a thread on its way is in this or in
   the run counts at every moment: they are read together, as of one instant
   (counts_snapshot, NTIMED_INC). */
/* (#8702) A sleeping thread is on a list of its own, under its own lock
   (sleep_wl): a timer that ends the sleep, a #wakeup and a #kill go to the
   thread itself, so no list of sleepers is shared and nothing sleeping
   contends on one. */
static void sleep_link(sp_thread *t) {   /* PRE: t->sleep_wl held */
  t->sleep_slot = t; t->wait_next = NULL;
  t->wait_head = &t->sleep_slot;
  SP_ATOMIC_STORE(&t->wait_lock, &t->sleep_wl, __ATOMIC_RELEASE);
}
static void sleep_unlink(sp_thread *t) {   /* PRE: t->sleep_wl held */
  t->sleep_slot = NULL; t->wait_next = NULL; t->wait_head = NULL;
  SP_ATOMIC_STORE(&t->wait_lock, NULL, __ATOMIC_RELEASE);
}
static sp_thread *g_io_waiters = NULL;    /* threads parked on a fd, woken by the monitor's poll */
/* The I/O waiter list is linked both ways (wait_next, io_prev): a server with
   a thread per connection has one waiter per connection, and every readiness
   event took its thread off by walking the list from the head, under the
   scheduler lock -- at 2048 connections that walk was most of the run. */
static void io_waiters_link(sp_thread *t) {
  t->io_prev = NULL; t->wait_next = g_io_waiters;
  if (g_io_waiters) g_io_waiters->io_prev = t;
  g_io_waiters = t; t->wait_head = &g_io_waiters;
}
static void io_waiters_unlink(sp_thread *t) {
  if (t->io_prev) t->io_prev->wait_next = t->wait_next; else g_io_waiters = t->wait_next;
  if (t->wait_next) t->wait_next->io_prev = t->io_prev;
  t->io_prev = NULL;
}
#ifdef SP_THREADS
/* A thread has parked on g_io_waiters: from then on a close visits the list
   (sp_sched_ev_forget). Set once, and never with the event backend up. */
static int g_io_polled = 0;
static void sched_make_runnable(sp_thread *t);   /* below */
/* Ready t, parked on g_io_waiters, with `rev` as its result. PRE: sched lock held. */
static void io_waiter_ready(sp_thread *t, short rev) {
  io_waiters_unlink(t);
  sp_timer_cancel(t);
  t->wait_next = NULL; t->wait_head = NULL; t->io_revents = rev; t->io_fd = -1;
  if (t == &g_main_thread) { SP_TH_SET_STATE(t, SP_TH_RUNNABLE); SCHED_WAKE_MAIN(); }
  else sched_make_runnable(t);
}
#endif
static struct pollfd *g_pfds = NULL;     /* monitor's poll set, rebuilt from g_io_waiters each tick */
static sp_thread    **g_pths = NULL;     /* parallel to g_pfds: the thread waiting on each fd */
static int            g_pcap = 0;        /* capacity of g_pfds / g_pths */

/* ---- persistent I/O registration (#4306 / #4317) -----------------------
   poll(2) is stateless: the monitor had to hand the WHOLE parked population to
   the kernel on every turn, so a wake cost O(parked) -- 109 us per turn at
   5,000 parked, against epoll_wait's flat 0.35.

   The kernel's interest set is keyed by DESCRIPTOR (epoll_ctl targets an fd,
   and a second ADD on the same one is EEXIST), so this table is too, and a
   readiness event fans out to every waiter on that descriptor. Two waiters on
   one fd is a shape that already runs: IO.select over a #to_io wrapper and the
   IO it wraps (test/io_select_to_io.rb).

   ONE-SHOT registration. An armed descriptor with no waiter would report
   readiness forever and spin the monitor, so the arm is EPOLLONESHOT: the
   kernel disables it after the one delivery, and a park re-arms with MOD.
   That is one syscall per park -- what the self-pipe write cost anyway -- and
   it removes the O(parked) term entirely.

   TEARDOWN is self-healing (matz's call), and the direction of failure is what
   makes that safe: the arm is attempted on EVERY park rather than skipped on
   the belief that a previous one still stands. A registration the kernel has
   dropped (its descriptor closed, or the number reused) answers ENOENT and is
   re-added; one that outlives its descriptor costs a wake nobody wants, which
   is discarded. What must never happen -- a park that arms nothing and waits
   forever -- cannot, because nothing is ever assumed still armed. */
#ifdef SP_EV_BACKEND
static int         g_ev_fd  = -1;
static unsigned long long g_ev_arms = 0, g_ev_adds = 0, g_ev_lost = 0, g_ev_timeouts = 0, g_ev_backstop = 0;
#endif

/* The handler-stack snapshot a green thread takes around each park (save,
   switch out, load). Parks are the hot path of a server -- one per wait for
   a socket -- and each one made a context with calloc, grew each of its
   handler arrays with realloc and freed them all on resume. A small pool per
   OS thread keeps them instead: a park takes one and its resume gives it
   back, on the same OS thread, since a started green thread runs only on
   its home worker (home_wid); past SP_SNAP_POOL a context is freed. What a
   pooled context held is never read again: save rewrites every count and
   the live prefix of every array, and load and the mark go by the counts.
   A build with C extensions keeps the extension arena in the context,
   which its free disposes of, so it takes a new context each time as
   before. */
#ifdef SP_CEXT
static void *sp_snap_take(void) { return sp_exc_ctx_new(); }
static void sp_snap_give(void *p) { sp_exc_ctx_free(p); }
#else
#define SP_SNAP_POOL 64
static SP_TLS void *g_snap_pool[SP_SNAP_POOL];
static SP_TLS int g_snap_n;
static void *sp_snap_take(void) { return g_snap_n ? g_snap_pool[--g_snap_n] : sp_exc_ctx_new(); }
static void sp_snap_give(void *p) {
  if (g_snap_n < SP_SNAP_POOL) g_snap_pool[g_snap_n++] = p;
  else sp_exc_ctx_free(p);
}
#endif
static sp_thread *g_all = NULL;          /* registry of live threads, for GC rooting */

#ifdef SP_EV_BACKEND
/* Bring up the event set once, lazily: a program with no I/O park never pays
   for it, and a kernel without epoll leaves g_ev_fd -1 and the poll path in
   place. */
typedef struct { int fd; short rev; } sp_ev_ready;
static int  sp_ev_backend_arm(int set, int fd, short want);
static void sp_ev_backend_del(int set, int fd);
static int  sp_ev_backend_wait(int set, sp_ev_ready *out, int max, int tmo_ms);

/* The wait's own timeout. Every wake has a kick or an event behind it, so this
   is a backstop and nothing routes through it; SPINEL_SCHED_STATS counts how
   often it expires, which should be "rarely" and is how the missing kick was
   found. */
#define SP_EV_BACKSTOP_MS 50
static int sp_ev_disabled(void) {
  static int asked = 0, off = 0;
  if (!asked) { const char *e = getenv("SPINEL_SCHED_POLL"); off = (e && *e && *e != '0'); asked = 1; }
  return off;
}
/* Each worker owns a readiness set holding the descriptors of the threads
   pinned to IT, so the worker that can run a ready thread is the one the
   kernel wakes -- no monitor round trip, no condvar hand-off (#4306). The set
   and its kick pipe are created on that worker's first park. */
static int sp_ev_worker_up(int wid) {   /* PRE: sched lock held */
  if (wid < 0 || wid >= SP_MAX_WORKERS) return 0;
  if (g_wslot[wid].evfd > 0) return 1;
  if (g_wslot[wid].evfd == -2 || sp_ev_disabled()) { SP_ATOMIC_STORE(&g_wslot[wid].evfd, -2, __ATOMIC_RELAXED); return 0; }
#ifdef SP_EV_EPOLL
  int fd = epoll_create1(EPOLL_CLOEXEC);
#else
  int fd = kqueue();
  if (fd >= 0) { int fl = fcntl(fd, F_GETFD); if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC); }
#endif
  if (fd < 0) { SP_ATOMIC_STORE(&g_wslot[wid].evfd, -2, __ATOMIC_RELAXED); return 0; }
  if (pipe(g_wslot[wid].kick) != 0) { close(fd); SP_ATOMIC_STORE(&g_wslot[wid].evfd, -2, __ATOMIC_RELAXED); return 0; }
  for (int i = 0; i < 2; i++) {
    int fl = fcntl(g_wslot[wid].kick[i], F_GETFL);
    if (fl >= 0) fcntl(g_wslot[wid].kick[i], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(g_wslot[wid].kick[i], F_GETFD);
    if (fl >= 0) fcntl(g_wslot[wid].kick[i], F_SETFD, fl | FD_CLOEXEC);
  }
  g_wslot[wid].kick_armed = 0;
  /* sequentially consistent: an I/O park that brings the set up reads the
     handle's closed flag after this, and sp_sched_ev_forget reads this after
     the flag (see there) */
  SP_ATOMIC_STORE(&g_wslot[wid].evfd, fd, __ATOMIC_SEQ_CST);
  g_ev_fd = fd;   /* any set being up is what tells the monitor the backend is live */
  return 1;
}
/* Re-arm the kick pipe. One-shot like everything else in the set. */
static void sp_ev_arm_kick(int wid) {   /* by worker wid itself: kick_armed is read and written by it alone */
  if (g_wslot[wid].evfd <= 0 || g_wslot[wid].kick_armed) return;
  sp_ev_backend_arm(g_wslot[wid].evfd, g_wslot[wid].kick[0], POLLIN);
  g_wslot[wid].kick_armed = 1;
}
/* Break a worker out of its readiness wait. A byte is enough; the reader
   drains whatever accumulated. */
static void sp_ev_kick(int wid) {   /* PRE: sched lock held */
  if (wid < 0 || wid >= SP_MAX_WORKERS || g_wslot[wid].evfd <= 0) return;
  char c = 1; ssize_t r = write(g_wslot[wid].kick[1], &c, 1); (void)r;
}

/* Arm one descriptor for `want` (POLLIN / POLLOUT), one-shot. 0 on success.
   The three functions below are the whole platform surface; everything above
   and below them is shared. */
static int sp_ev_backend_arm(int set, int fd, short want) {
#ifdef SP_EV_EPOLL
  struct epoll_event e;
  e.events = (uint32_t)((want & POLLIN ? EPOLLIN : 0) | (want & POLLOUT ? EPOLLOUT : 0)) | EPOLLONESHOT;
  e.data.fd = fd;
  if (epoll_ctl(set, EPOLL_CTL_MOD, fd, &e) == 0) return 0;
  /* Not in the set: epoll's one-shot only DISABLES, so MOD is the usual arm
     and ADD is the first one. */
  if (errno == ENOENT) { SP_STAT_ADD(g_ev_adds, 1); return epoll_ctl(set, EPOLL_CTL_ADD, fd, &e); }
  return -1;
#else
  /* kqueue's one-shot DELETES the entry when it fires, so every arm is an
     EV_ADD -- and a filter the waiters no longer want stays armed until it
     fires once into nobody, which is the discarded wake the design allows. */
  struct kevent ch[2];
  int n = 0;
  if (want & POLLIN)  EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ,  EV_ADD | EV_ONESHOT, 0, 0, NULL);
  if (want & POLLOUT) EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
  if (!n) return 0;
  SP_STAT_ADD(g_ev_adds, 1);
  return kevent(set, ch, n, NULL, 0, NULL) < 0 ? -1 : 0;
#endif
}

/* Drop a descriptor from the set outright (a handle is closing). */
static void sp_ev_backend_del(int set, int fd) {
#ifdef SP_EV_EPOLL
  epoll_ctl(set, EPOLL_CTL_DEL, fd, NULL);
#else
  struct kevent ch[2];
  EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
  EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  kevent(set, ch, 2, NULL, 0, NULL);   /* ENOENT for a filter never armed */
#endif
}

/* Wait for readiness. Fills `out` with (descriptor, poll-style events) and
   answers how many, or a negative on error. */
static int sp_ev_backend_wait(int set, sp_ev_ready *out, int max, int tmo_ms) {
#ifdef SP_EV_EPOLL
  struct epoll_event evs[64];
  if (max > 64) max = 64;
  int n = epoll_wait(set, evs, max, tmo_ms);
  for (int i = 0; i < n; i++) {
    out[i].fd = evs[i].data.fd;
    out[i].rev = (short)(((evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) ? POLLIN : 0) |
                         ((evs[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) ? POLLOUT : 0));
  }
  return n;
#else
  struct kevent evs[64];
  if (max > 64) max = 64;
  struct timespec ts;
  ts.tv_sec = tmo_ms / 1000;
  ts.tv_nsec = (long)(tmo_ms % 1000) * 1000000L;
  int n = kevent(set, NULL, 0, evs, max, &ts);
  for (int i = 0; i < n; i++) {
    out[i].fd = (int)evs[i].ident;
    /* EV_EOF is a peer that closed: readable (and writable, for a socket whose
       other end is gone) exactly as POLLHUP is. EV_ERROR answers both so the
       waiter's own read or write reports the error, which is what poll did. */
    short r = (evs[i].filter == EVFILT_WRITE) ? POLLOUT : POLLIN;
    if (evs[i].flags & (EV_EOF | EV_ERROR)) r = POLLIN | POLLOUT;
    out[i].rev = r;
  }
  return n;
#endif
}
/* Arm `fd` for the union of what its waiters want. Attempted on every park --
   see the note above: never skipped on the belief that an earlier arm stands. */
/* (#8702) The I/O a thread waits on is the business of the worker it is pinned
   to (home_wid): its descriptors are armed in that worker's readiness set, and
   only that worker's drain readies it. So the waiters live with the worker,
   under the worker's own I/O lock (io_wl, a wait lock), not the scheduler
   lock: the thread's entry on the worker's I/O waiter list (io_waiters, linked
   both ways, wait_next / io_prev), and each of its descriptor entries in the
   worker's table (ev_tab, indexed by descriptor). A park, the drain that
   readies it and the wake take no global lock; a #kill, #raise or timeout
   unlinks it under io_wl like any wait lock (sp_sched_unpark). NIO_INC counts
   the threads parked this way, for the waits that ask whether any I/O is
   outstanding. Without the event backend (SPINEL_SCHED_POLL, or no epoll or
   kqueue) the monitor polls g_io_waiters under the scheduler lock instead. */
/* (threads parked on I/O in a worker's set: NIO_INC, with the run counts) */
static void sched_wake_unlinked(sp_thread *t);
static int sp_ev_home_of(sp_thread *t) { return t->home_wid < 0 ? 0 : (int)t->home_wid; }
/* What an entry waits for: the thread's own descriptor, or one of its set */
static short sp_evw_events(sp_ev_waiter *e) { return e->idx < 0 ? e->t->io_events : e->t->io_set[e->idx].events; }
/* Is t parked on worker w's I/O list? (under w's io_wl, or t's own) */
static int sp_io_parked_on(sp_thread *t, int w) { return t->wait_head == &g_wslot[w].io_waiters; }
static void wio_link(int w, sp_thread *t) {   /* PRE: w's io_wl held */
  sp_wslot *s = &g_wslot[w];
  t->io_prev = NULL; t->wait_next = s->io_waiters;
  if (s->io_waiters) s->io_waiters->io_prev = t;
  s->io_waiters = t;
  t->wait_head = &s->io_waiters;
  SP_ATOMIC_STORE(&t->wait_lock, &s->io_wl, __ATOMIC_RELEASE);
  NIO_INC();
}
static void wio_unlink(int w, sp_thread *t) {   /* PRE: w's io_wl held */
  sp_wslot *s = &g_wslot[w];
  if (t->io_prev) t->io_prev->wait_next = t->wait_next; else s->io_waiters = t->wait_next;
  if (t->wait_next) t->wait_next->io_prev = t->io_prev;
  t->io_prev = NULL; t->wait_next = NULL; t->wait_head = NULL;
  SP_ATOMIC_STORE(&t->wait_lock, NULL, __ATOMIC_RELEASE);
  NIO_DEC();
}
/* Arm `fd` in worker w's set for the union of what its waiters there want.
   Attempted on every park -- see the note above: never skipped on the belief
   that an earlier arm stands. PRE: w's io_wl held. */
static void sp_ev_arm_fd_w(int w, int fd) {
  sp_wslot *s = &g_wslot[w];
  if (fd < 0 || fd >= s->ev_cap || s->evfd <= 0) return;
  short want = 0;
  for (sp_ev_waiter *x = s->ev_tab[fd]; x; x = x->next) want |= sp_evw_events(x);
  if (!want) return;
  SP_STAT_ADD(g_ev_arms, 1);
  /* EBADF, or a descriptor the kernel will not watch (epoll refuses a regular
     file): the waiter falls back on its deadline, which is the same answer
     poll gave for an always-ready file. Counted, so SPINEL_SCHED_STATS shows
     it rather than leaving a silent gap. */
  if (sp_ev_backend_arm(s->evfd, fd, want) != 0) SP_STAT_ADD(g_ev_lost, 1);
}
/* Put entry e on descriptor fd in worker w's table and arm it. Returns 0 when
   the table could not grow: the waiter is then on its deadline alone, the
   same degradation the poll path has when its own array cannot grow.
   PRE: w's io_wl held. */
static int sp_ev_park_entry_w(int w, sp_ev_waiter *e, int fd) {
  sp_wslot *s = &g_wslot[w];
  if (fd < 0) return 0;
  if (fd >= s->ev_cap) {
    int nc = s->ev_cap ? s->ev_cap : 64;
    while (nc <= fd) nc *= 2;
    sp_ev_waiter **nt = (sp_ev_waiter **)realloc(s->ev_tab, sizeof(sp_ev_waiter *) * (size_t)nc);
    if (!nt) return 0;
    memset(nt + s->ev_cap, 0, sizeof(sp_ev_waiter *) * (size_t)(nc - s->ev_cap));
    s->ev_tab = nt; s->ev_cap = nc;
  }
  e->next = s->ev_tab[fd];
  s->ev_tab[fd] = e;
  sp_ev_arm_fd_w(w, fd);
  return 1;
}
static void sp_ev_unlink_w(int w, sp_ev_waiter *e, int fd) {   /* PRE: w's io_wl held */
  sp_wslot *s = &g_wslot[w];
  if (fd >= 0 && fd < s->ev_cap)
    for (sp_ev_waiter **pp = &s->ev_tab[fd]; *pp; pp = &(*pp)->next)
      if (*pp == e) { *pp = e->next; break; }
  e->next = NULL;
}
/* Take t off worker w's descriptor table and its I/O list: its own
   descriptor, and every entry of a set wait. PRE: w's io_wl held. */
static void sp_ev_drop_w(int w, sp_thread *t) {
  sp_ev_unlink_w(w, &t->ev0, t->io_fd);
  for (int i = 0; t->ev_set && i < t->io_nset; i++) sp_ev_unlink_w(w, &t->ev_set[i], t->io_set[i].fd);
  wio_unlink(w, t);
}
/* Hand one readiness event on worker w to the threads waiting there on that
   descriptor: each is taken off the table and the list and put in `out`, to
   be woken once io_wl is dropped (sp_ev_wake_out), since a wake can take the
   scheduler lock (main, a timer to cancel). Re-arms for whoever is left, as
   the arm is one-shot and a waiter still parked must not be stranded.
   Answers how many went into `out`. PRE: w's io_wl held. */
#define SP_EV_OUT 64
static int sp_ev_dispatch_w(int w, int fd, short rev, sp_thread **out, int n) {
  sp_wslot *s = &g_wslot[w];
  if (fd < 0 || fd >= s->ev_cap) return n;
  for (sp_ev_waiter *e = s->ev_tab[fd], *nx = NULL; e && n < SP_EV_OUT; e = nx) {
    nx = e->next;
    sp_thread *t = e->t;
    if (!(sp_evw_events(e) & rev) || !sp_io_parked_on(t, w)) continue;
    SP_STAT_ADD(g_mon_readied, 1);
    /* still counted as an I/O waiter until its wake has counted it runnable
       (sp_ev_wake_out): main's pump reads the counts to decide that nothing
       can wake it, and a thread in neither looked like a deadlock. Raised
       before the drop lowers it, so the count never passes through zero. */
    NIO_INC();
    sp_ev_drop_w(w, t);
    t->io_revents = rev; t->io_fd = -1;
    out[n++] = t;
  }
  sp_ev_arm_fd_w(w, fd);
  return n;
}
static void sp_ev_wake_out(sp_thread **out, int n) {   /* no io_wl held */
  for (int i = 0; i < n; i++) {
    sched_wake_unlinked(out[i]);
    NIO_DEC();   /* see sp_ev_dispatch_w */
  }
}
/* Handle what one wait on worker w's set returned: the kick pipe is drained,
   and every other descriptor readies its waiters. No scheduler lock needed;
   the wakes take it where they must. Answers how many threads it readied. */
static int sp_ev_handle(int w, sp_ev_ready *evs, int en) {
  sp_thread *out[SP_EV_OUT]; int n = 0, total = 0;
  for (int i = 0; i < en; i++) {
    if (evs[i].fd == g_wslot[w].kick[0]) {
      char buf[64]; while (read(g_wslot[w].kick[0], buf, sizeof buf) > 0) {}
      g_wslot[w].kick_armed = 0;
      continue;
    }
    wl_lock(&g_wslot[w].io_wl);
    n = sp_ev_dispatch_w(w, evs[i].fd, evs[i].rev, out, 0);
    wl_unlock(&g_wslot[w].io_wl);
    sp_ev_wake_out(out, n);
    total += n;
  }
  return total;
}
/* A zero-timeout look at this worker's own set, from any lock context: what
   is ready is readied now. The worker's fast path takes it between threads
   it runs, which is what lets a busy worker deliver I/O without the scheduler
   lock. */
/* When this worker last looked at its set, and how many threads it has run
   since; read by sp_ev_drain_due. */
static SP_TLS unsigned g_ev_runs_since;
static SP_TLS double g_ev_drained_at;
static int sp_ev_drain(int wid) {
  if (g_wslot[wid].evfd <= 0) return 0;
  g_ev_runs_since = 0;
  g_ev_drained_at = sp_monotonic_now();
  sp_ev_ready evs[64];
  int en = sp_ev_backend_wait(g_wslot[wid].evfd, evs, 64, 0);
  if (en <= 0) return 0;
  SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, en);
  return sp_ev_handle(wid, evs, en);
}

/* Whether a worker that is about to run another thread looks at its set
   first. With its own queue empty it always does, as before. With threads
   queued, it does every SP_EV_DRAIN_RUNS threads, or once SP_EV_DRAIN_GAP
   seconds have passed since the last look, so a parked thread whose
   descriptor is ready waits a bounded time instead of a look being paid
   on every thread switch. */
#define SP_EV_DRAIN_RUNS 16u
#define SP_EV_DRAIN_GAP  0.001
static int sp_ev_drain_due(int wid) {
  if (!SP_ATOMIC_LOAD(&g_lrq[wid].head, __ATOMIC_ACQUIRE)) return 1;
  if (++g_ev_runs_since >= SP_EV_DRAIN_RUNS) return 1;
  return sp_monotonic_now() - g_ev_drained_at >= SP_EV_DRAIN_GAP;
}

/* Wait on this worker's own readiness set for up to `tmo_ms`, then ready what
   came back -- before taking the scheduler lock again, since readying takes
   only the worker's own lock: a worker woken by an event used to queue on the
   scheduler lock just to hand it out. Answers how many threads it readied.
   PRE/POST: sched lock held. */
static int sp_ev_worker_wait(int wid, int tmo_ms) {
  if (g_wslot[wid].evfd <= 0) return 0;
  if (tmo_ms == 0) {   /* the per-turn drain: nothing to leave or re-enter */
    SCHED_UNLOCK();
    int n = sp_ev_drain(wid);
    SCHED_LOCK();
    return n;
  }
  sp_ev_ready evs[64];
  sp_ev_arm_kick(wid);
  g_wslot[wid].ev_waiting = 1;
  int set = g_wslot[wid].evfd;
  /* the idle wait leaves the world: a collection raised meanwhile marks this
     worker's recorded fibers and does not wake it to park. Readying the
     threads it was woken for happens while still out of it: a wake moves
     pointers between lists and queues and runs no Ruby. */
  int out = !g_stw_active;
  if (out) sp_out_enter_locked(wid, SP_OUT_IDLE);
  SP_ATOMIC_STORE(&g_wslot[wid].parked, 1, __ATOMIC_SEQ_CST); SP_ATOMIC_STORE(&g_wslot[wid].wake_sent, 0, __ATOMIC_RELAXED);
  SCHED_UNLOCK();
  int en = sp_ev_backend_wait(set, evs, 64, tmo_ms);
  if (en == 0) SP_STAT_ADD(g_ev_backstop, 1);
  SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, en > 0 ? en : 0);
  int n = en > 0 ? sp_ev_handle(wid, evs, en) : 0;
  SCHED_LOCK();
  if (out) sp_out_leave_locked(wid);
  g_wslot[wid].ev_waiting = 0;
  SP_ATOMIC_STORE(&g_wslot[wid].parked, 0, __ATOMIC_RELAXED);
  return n;
}

/* A handle is closing: the descriptor is about to stop being ours, so drop the
   registration while the fd still names the right thing. Whoever is parked on
   it is readied first: a thread blocked in a read has no deadline, so with its
   registration gone and its entry dropped nothing would ever ready it --
   `sock.close` from another thread left the reader waiting forever, where
   CRuby raises IOError in it (#4546). Readied here as for any readiness event;
   the read it retries finds the handle closed and raises. Every worker's
   table is visited under that worker's own lock: a waiter parks under it
   after looking at the handle's closed flag, which is written before this.
   A worker whose set is not up has no table to visit. But a park may be
   bringing that set up as the flag is written: it stores the set, then reads
   the flag, while this wrote the flag and then reads the set. The fence and
   the sequentially consistent accesses on both sides make one of the two see
   the other's store: the park sees the flag and does not wait, or this sees
   the set and readies the park under the worker's lock. */
void sp_sched_ev_forget(int fd) {
  if (fd < 0) return;
  SP_ATOMIC_FENCE(__ATOMIC_SEQ_CST);
  for (int w = 0; w < SP_MAX_WORKERS; w++) {
    if (SP_ATOMIC_LOAD(&g_wslot[w].evfd, __ATOMIC_SEQ_CST) <= 0) continue;
    sp_thread *out[SP_EV_OUT]; int n;
    do {
      wl_lock(&g_wslot[w].io_wl);
      n = sp_ev_dispatch_w(w, fd, POLLIN | POLLOUT, out, 0);
      if (n < SP_EV_OUT) {
        sp_ev_backend_del(g_wslot[w].evfd, fd);
        if (fd < g_wslot[w].ev_cap) {
          for (sp_ev_waiter *e = g_wslot[w].ev_tab[fd]; e; ) { sp_ev_waiter *nx = e->next; e->next = NULL; e = nx; }
          g_wslot[w].ev_tab[fd] = NULL;
        }
      }
      wl_unlock(&g_wslot[w].io_wl);
      sp_ev_wake_out(out, n);
    } while (n == SP_EV_OUT);
  }
  /* Without the event backend (SPINEL_SCHED_POLL, or a set that could not be
     made) a waiter on the descriptor is on g_io_waiters, for the monitor's
     poll(), and no event comes for it: a close leaves the descriptor open
     while a thread still uses the stream, so poll() does not find it invalid
     either, and an accept waiting for the next connection waited for good.
     Ready it under the scheduler lock that list is under. The park raises
     g_io_polled and then reads the handle's closed flag; this wrote the flag,
     fenced, and reads g_io_polled: so the park sees the flag and does not
     wait, or this takes the lock, which the park holds from its look at the
     flag until it is on the list. */
  if (SP_ATOMIC_LOAD(&g_io_polled, __ATOMIC_SEQ_CST)) {
    SCHED_LOCK();
    for (sp_thread *t = g_io_waiters, *nx; t; t = nx) {
      nx = t->wait_next;
      int hit = t->io_fd == fd;
      for (int i = 0; !hit && t->io_fd < 0 && i < t->io_nset; i++) hit = t->io_set[i].fd == fd;
      if (hit) io_waiter_ready(t, POLLIN | POLLOUT);
    }
    SCHED_UNLOCK();
  }
}
#else
void sp_sched_ev_forget(int fd) { (void)fd; }
/* no worker parks a thread on I/O without the event backend (Windows) */
#endif

static unsigned   g_next_id = 1;
static void sp_thread_raise_main_locked(const char *cls, const char *msg, void *obj);
static unsigned char g_report_default = 1;  /* Thread.report_on_exception default */

#ifdef SP_THREADS
static void sp_sched_maybe_grow(void);   /* grow the helper pool toward demand */
#endif
/* SPINEL_SCHED_STATS=2: how long a readied thread waited on a run queue before
   a worker ran it, as a log2 histogram (bucket k: [2^k, 2^(k+1)) us), reported
   by the monitor every few seconds. The scheduling delay is what a request's
   tail latency is made of when the CPU is not the bottleneck. */
static int g_sched_lat_on = -1;
static unsigned long long g_sched_lat_hist[32];
static double g_sched_lat_max = 0;
static unsigned long long g_sched_lat_n = 0;
static int g_lrq_len[SP_MAX_WORKERS], g_lrq_len_max = 0;
static unsigned long long g_mtx_hist[32], g_mtx_n = 0, g_mtx_fast = 0, g_mtx_spin = 0; static double g_mtx_max = 0;
static unsigned long long g_cv_hist[32], g_cv_n = 0; static double g_cv_max = 0;
static void sched_hist_add(unsigned long long *h, unsigned long long *n, double *mx, double us) {
  int k = 0; while (k < 31 && us >= (double)(2u << k)) k++;
  h[k]++; (*n)++; if (us > *mx) *mx = us;
}
static void sched_hist_print(const char *tag, unsigned long long *h, unsigned long long n, double mx) {
  fprintf(stderr, "[%s] n=%llu max=%.0fus  us:", tag, n, mx);
  for (int k = 0; k < 32; k++) if (h[k]) fprintf(stderr, " [%u]=%llu", 1u << k, h[k]);
  fprintf(stderr, "\n");
}
static int sched_lat_enabled(void) {
  if (g_sched_lat_on < 0) { const char *e = getenv("SPINEL_SCHED_STATS"); g_sched_lat_on = (e && *e == '2'); }
  return g_sched_lat_on;
}
#ifdef SP_THREADS
/* (#8702) Unstarted threads waiting on some worker's own queue: the ones an
   idle worker may steal. A worker idling without the scheduler lock looks for
   them only while this is nonzero, so a busy server's queues of pinned
   threads are not scanned by every idle worker in turn. */
static int g_lrq_unstarted = 0;
#endif
static void runq_push(sp_runq *q, sp_thread *t) {
  SP_TH_SET_STATE(t, SP_TH_RUNNABLE); t->rq_next = NULL;
  if (sched_lat_enabled()) t->readied_at = sp_monotonic_now();
  /* head is stored atomically: a stealing worker peeks at it without the
     queue's lock (sched_pick) */
  if (q->tail) q->tail->rq_next = t; else SP_ATOMIC_STORE(&q->head, t, __ATOMIC_RELAXED);
  q->tail = t; RUNNABLE_INC();
#ifdef SP_THREADS
  if (q != &g_grq && t->home_wid < 0) SP_ATOMIC_FETCH_ADD(&g_lrq_unstarted, 1, __ATOMIC_SEQ_CST);
  if (q == &g_grq) sp_sched_maybe_grow();   /* one execution helper per live green thread, up to the cap */
#endif
}
#ifdef SP_THREADS
/* SPINEL_WAKE_PREEMPT: a woken thread goes ahead of the threads queued here
   when their timeslice ended, behind those woken before it. Only a worker's
   own queue, and only a started thread, which nothing steals: the woken ones
   leave from the front. PRE: the queue's lock held. */
static void runq_push_woken(sp_runq *q, sp_thread *t) {
  SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
  if (sched_lat_enabled()) t->readied_at = sp_monotonic_now();
  sp_thread *p = q->woke;
  if (p) { t->rq_next = p->rq_next; p->rq_next = t; }
  else { t->rq_next = q->head; SP_ATOMIC_STORE(&q->head, t, __ATOMIC_RELAXED); }
  if (q->tail == p) q->tail = t;
  q->woke = t; RUNNABLE_INC();
}
#endif
/* Take the first thread off q for this worker to run now: as runq_pop, but
   the thread stays counted runnable until run_thread_once counts it running,
   in one change (g_run_taken), so it is never in neither count. */
static SP_TLS int g_run_taken = 0;
static sp_thread *runq_take(sp_runq *q) {
  sp_thread *t = q->head;
  if (t) { SP_ATOMIC_STORE(&q->head, t->rq_next, __ATOMIC_RELAXED); if (!q->head) q->tail = NULL; t->rq_next = NULL;
    if (q->woke == t) q->woke = NULL;
#ifdef SP_THREADS
    /* an unstarted thread a thread spawned onto this worker's own queue
       (sp_Thread_spawn_fiber): without this the count stayed up, and every
       idle worker scanned the queues and never waited */
    if (q != &g_grq && t->home_wid < 0) SP_ATOMIC_FETCH_SUB(&g_lrq_unstarted, 1, __ATOMIC_RELAXED);
#endif
  }
  return t;
}
static sp_thread *runq_pop(sp_runq *q) {
  sp_thread *t = q->head;
  if (t) { SP_ATOMIC_STORE(&q->head, t->rq_next, __ATOMIC_RELAXED); if (!q->head) q->tail = NULL; t->rq_next = NULL; RUNNABLE_DEC();
    if (q->woke == t) q->woke = NULL;
#ifdef SP_THREADS
    if (q != &g_grq && t->home_wid < 0) SP_ATOMIC_FETCH_SUB(&g_lrq_unstarted, 1, __ATOMIC_RELAXED);
#endif
  }
  return t;
}
/* Spawn: the shared global queue (no worker affinity yet). */
static void rq_push(sp_thread *t)           { runq_push(&g_grq, t); }
/* Rerun a thread on the worker that just ran it (locality). */
static void lrq_push(int wid, sp_thread *t) { RQ_LOCK(wid); runq_push(&g_lrq[wid], t); RQ_UNLOCK(wid); }
/* Requeue/wake a thread that may have already run. A STARTED thread must go
   back to its home worker's local queue -- never the global queue -- because
   its live frames cache that worker's __thread addresses (see home_wid). An
   unstarted thread has no affinity and takes the global queue. */
static void runq_requeue(sp_thread *t) {
#ifdef SP_THREADS
  if (t->home_wid >= 0) { lrq_push(t->home_wid, t); return; }
#endif
  runq_push(&g_grq, t);
}
/* Next thread for worker `wid`: its own queue, then the global queue, then steal
   one from another worker. Every 61st pick checks the global queue first so a
   worker that keeps refilling its own queue (e.g. a thread spawning in a loop)
   cannot starve globally-requeued (preempted / woken) work. */
static SP_TLS unsigned g_pick_tick = 0;
/* Set when the thread this worker just ran was switched out at the end of
   its quantum (sp_safepoint_preempt). Such a run took a whole quantum, so
   picking from the local queue again would leave the global queue's
   unstarted threads a 61-pick wait of one quantum each. */
static SP_TLS int g_run_preempted = 0;
#ifdef SP_THREADS
/* An unstarted thread popped from the global queue is about to be pinned to
   whichever worker runs it first, for life. Under a helper pool that grows on
   demand the workers that exist when a server's connections arrive are the
   busy ones, so every connection was pinned to the first dozen workers and
   the rest sat idle for the run: 64 connections on 12 of 32 workers, with 7
   queued on one while 21 workers had nothing (campfire at c=64). Place it on
   the worker with the fewest pinned threads instead -- spawning one more
   helper when every worker already carries some and the cap allows -- and
   hand it over there. Returns 1 when t was given away. */
static void sp_sched_spawn_helper(void);
static int sched_place_unstarted(int wid, sp_thread *t) {
  if (t->home_wid >= 0 || sp_active_workers <= 1) return 0;
  int best = -1, bestn = 0;
  for (int i = 1; i < sp_active_workers; i++) {   /* worker 0 (main) runs no general thread */
    if (!g_wslot[i].active) continue;
    int np = SP_ATOMIC_LOAD(&g_wslot[i].pinned, __ATOMIC_RELAXED);
    if (best < 0 || np < bestn) { best = i; bestn = np; }
  }
  if (best < 0) return 0;
  if (bestn > 0 && g_helpers_spawned < g_worker_cap && !g_stw_active) {
    int before = g_helpers_spawned;
    sp_sched_spawn_helper();
    if (g_helpers_spawned > before) { best = g_helpers_spawned; bestn = 0; }
  }
  if (best == wid || (wid > 0 && SP_ATOMIC_LOAD(&g_wslot[wid].pinned, __ATOMIC_RELAXED) <= bestn)) return 0;   /* here is as good */
  t->home_wid = (short)best; SP_ATOMIC_FETCH_ADD(&g_wslot[best].pinned, 1, __ATOMIC_RELAXED);
  lrq_push(best, t);
  sched_wake_home(best);
  return 1;
}
#endif
#ifdef SP_THREADS
/* Steal one unstarted thread from another worker's queue, or answer NULL.
   Takes only the queues' own locks: the scheduler lock is held or not. With
   `take`, the thread stays counted runnable for run_thread_once to count
   running in one change, as runq_take's do. */
static sp_thread *sched_steal(int wid, int take) {
  if (!SP_ATOMIC_LOAD(&g_lrq_unstarted, __ATOMIC_SEQ_CST)) return NULL;   /* none to find */
  int nw = SP_ATOMIC_LOAD(&sp_active_workers, __ATOMIC_ACQUIRE);
  for (int i = 0; i < nw; i++) {   /* steal one from a busier worker */
    if (i == wid) continue;
    /* An empty queue is passed over without its lock: on a many-core box
       that was a lock and an unlock of every other worker's queue, under the
       scheduler lock, each time a worker found nothing to run. A thread
       pushed after the look is not lost -- whoever pushed it woke its home. */
    if (!SP_ATOMIC_LOAD(&g_lrq[i].head, __ATOMIC_RELAXED)) continue;
    /* Only an UNSTARTED thread may be stolen: a started one is pinned to its
       home worker's TLS (home_wid) and must not resume elsewhere. Unstarted
       spawns sit at most one-per-spawner ahead of pinned reruns, so scan the
       queue rather than popping blindly. */
    RQ_LOCK(i);
    sp_thread **pp = &g_lrq[i].head;
    while (*pp && (*pp)->home_wid >= 0) pp = &(*pp)->rq_next;
    if (*pp) {
      sp_thread *t = *pp;
      SP_ATOMIC_STORE(pp, t->rq_next, __ATOMIC_RELAXED);   /* pp may be the head the peek reads */
      if (g_lrq[i].tail == t) {
        g_lrq[i].tail = NULL;
        for (sp_thread *w = g_lrq[i].head; w; w = w->rq_next) g_lrq[i].tail = w;
      }
      t->rq_next = NULL; if (!take) RUNNABLE_DEC();
      SP_ATOMIC_FETCH_SUB(&g_lrq_unstarted, 1, __ATOMIC_RELAXED);
      RQ_UNLOCK(i);
      return t;
    }
    RQ_UNLOCK(i);
  }
  return NULL;
}
#endif
static sp_thread *sched_pick(int wid) {
  sp_thread *t;
#ifdef SP_THREADS
  /* the global queue holds unstarted threads: place each on its home first */
  while ((t = g_grq.head) != NULL && t->home_wid < 0) {
    runq_pop(&g_grq);
    if (!sched_place_unstarted(wid, t)) { return t; }
  }
#endif
  if ((++g_pick_tick % 61u) == 0) { t = runq_pop(&g_grq); if (t) return t; }
  RQ_LOCK(wid); t = runq_pop(&g_lrq[wid]); RQ_UNLOCK(wid);
  if (t) return t;
  t = runq_pop(&g_grq);
  if (t) return t;
#ifdef SP_THREADS
  if ((t = sched_steal(wid, 0)) != NULL) return t;
#endif
  return NULL;
}

/* Wake worker(s) after enqueueing t. A STARTED thread is pinned to its home
   worker (see home_wid) and sched_pick's stealing skips pinned threads, so the
   wake has exactly one worker to reach; an unstarted thread can run anywhere.
   This used to have to BROADCAST for a pinned thread, because every idle
   worker waited on the one g_sched_work condvar and a plain signal could take
   a worker that cannot run t -- see the per-worker condvar in sp_wslot, which
   is what lets the wake be addressed (#4305). PRE: sched lock held. */
static void sp_sched_wake_for(sp_thread *t) {
  sched_wake_home(t->home_wid);
}

/* (#8702) make a parked thread runnable, from any lock context
   but its home worker's run-queue lock. Exactly one of two transitions wins:
   PARKED -> RUN, and the waker queues it on its home worker and kicks that
   worker; or PARKING -> WOKEN, and the worker it is switching out to queues it
   once it is off its stack (run_thread_once). A thread already RUN or WOKEN has
   been woken. The home worker's queue is under its own lock, and its `idle`
   flag with it; main's worker (0) waits on the scheduler lock's condvar, so a
   push there goes through that lock, as does a thread that never ran (the
   global queue). */
static void sched_push_wake(sp_thread *t) {
#ifndef SP_THREADS
  runq_requeue(t);
#else
  int w = t->home_wid;
  if (w <= 0) {
    int had = g_sched_held;
    if (!had) SCHED_LOCK();
    runq_requeue(t);
    sp_sched_wake_for(t);
    if (!had) SCHED_UNLOCK();
    return;
  }
  RQ_LOCK(w);
  if (g_wake_preempt) runq_push_woken(&g_lrq[w], t); else runq_push(&g_lrq[w], t);
  if (g_wslot[w].evfd <= 0 && g_wslot[w].idle) pthread_cond_signal(&g_wslot[w].cv);
  RQ_UNLOCK(w);
#ifdef SP_EV_BACKEND
  if (g_wslot[w].evfd > 0 && w != sp_worker_id) sp_ev_kick(w);
#endif
  /* SPINEL_WAKE_PREEMPT: and the thread running on that worker yields to it,
     unless that is the waker, or t itself, taken already */
  if (g_wake_preempt && w != sp_worker_id) {
    sp_thread *r = SP_ATOMIC_LOAD(&g_wslot[w].cur, __ATOMIC_ACQUIRE);
    if (r && r != t && !SP_ATOMIC_LOAD(&r->preempt_request, __ATOMIC_ACQUIRE)) {
      int had = g_sched_held;
      if (!had) SCHED_LOCK();
      sched_preempt(w, r);
      if (!had) SCHED_UNLOCK();
    }
  }
#endif
}
static void sched_make_runnable(sp_thread *t) {
  for (;;) {
    int p = SP_ATOMIC_LOAD(&t->park, __ATOMIC_SEQ_CST);
    int e = p;
    if (p == SP_PK_PARKED) {
      if (SP_ATOMIC_CAS(&t->park, &e, SP_PK_RUN, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
        sched_push_wake(t);
        return;
      }
    }
    else if (p == SP_PK_PARKING) {
      if (SP_ATOMIC_CAS(&t->park, &e, SP_PK_WOKEN, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    }
    else return;
  }
}

/* (#8702) the waiter lists of a Mutex and a ConditionVariable are
   guarded by the object's own wait lock (sp_wlock), not the scheduler lock.
   Take the first waiter off such a list; the caller holds its lock. */
static sp_thread *wl_take_one(sp_thread **waitlist) {
  sp_thread *t = *waitlist;
  if (!t) return NULL;
  *waitlist = t->wait_next;
  t->wait_next = NULL; t->wait_head = NULL; SP_ATOMIC_STORE(&t->wait_lock, NULL, __ATOMIC_RELEASE);
  return t;
}
/* Make a waiter that is off its list runnable; no wait lock held. Main waits
   in its pump under the scheduler lock, so its wake goes through that lock.
   A timed wait's timer is cancelled under the timer lock. */
static void sched_wake_unlinked(sp_thread *t) {
#ifdef SP_THREADS
  sp_timer_cancel(t);
#endif
  if (t == &g_main_thread) {
    int had = g_sched_held;
    if (!had) SCHED_LOCK();
    /* Only a main still BLOCKED: it was taken off its list before this lock,
       and a #raise delivered in between (sp_thread_deliver) may have woken it
       and let it run on. Marked RUNNABLE again then, a running main would make
       its next pump -- the exit drain's, say -- return at once. Main sets
       BLOCKED before it goes on any list, so a wake owed to it finds it so. */
    if (SP_TH_STATE(t) == SP_TH_BLOCKED) SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
    SCHED_WAKE_MAIN();
    if (!had) SCHED_UNLOCK();
    return;
  }
  sched_make_runnable(t);
}
/* Wake one thread parked in m's #lock, if any; m's wait lock not held. */
static void sp_mutex_wake_waiter(sp_mutex *m) {
  wl_lock(&m->wl);
  sp_thread *t = wl_take_one(&m->waiters);
  wl_unlock(&m->wl);
  if (t) sched_wake_unlinked(t);
}

static void reg_add(sp_thread *t) {
  REG_LOCK();
  t->all_prev = NULL; t->all_next = g_all;
  if (g_all) g_all->all_prev = t;
  g_all = t;
  REG_UNLOCK();
}
/* Is any green thread other than the caller alive? A blocking syscall answers
   for the OS thread it runs on, and a started green thread is pinned to its
   worker, so blocking in one stalls every thread pinned there -- including the
   one that has to make progress before the syscall can return. Process.waitpid2
   and Kernel#system ask this before choosing between a blocking wait and a
   polling one (#4381).

   Main is not in the registry, so a green thread asking this saw "nobody
   else" once every other spawned thread had ended and took the blocking
   wait -- with main alive and allocating, and a worker in a syscall never
   reaches a safepoint, so main's next collection waited out the child
   (#4528). From a green thread the answer is always yes. */
int sp_sched_other_threads_live(void) {
  int other = 0;
  SCHED_LOCK();
  if (g_current != &g_main_thread) other = 1;
  else { REG_LOCK(); sp_thread *t = g_all;
    while (t) { if (t != g_current) { other = 1; break; } t = t->all_next; }
    REG_UNLOCK(); }
  SCHED_UNLOCK();
  return other;
}
/* Wait for one child without holding the OS worker while other green threads
   have to run. A blocking waitpid answers for the whole worker, and a started
   green thread is pinned to its worker, so `spin build --verbose` deadlocked:
   the parent waited for the compiler, the compiler filled the stderr pipe and
   blocked writing, and the reader thread that would have drained it could not
   be scheduled (#4381). The cooperative build has the same shape with one OS
   thread and several fibers.

   Poll and hand the scheduler back between attempts, but only while another
   thread is alive -- a program with one thread keeps the blocking wait and its
   exact wake-up. sp_Thread_pass runs a runnable sibling immediately; the sleep
   only keeps a busy loop from burning the core while the child works, and it
   caps at 5ms, which is nothing against a wait long enough to matter. */
int sp_sched_wait_child(int pid, int *status) { return sp_sched_wait_child_flags(pid, status, 0); }
/* The same with the flags of waitpid(2): WNOHANG asks once and answers 0 for
   a child still running, with no wait and no poll; WUNTRACED takes a stopped
   child too. The poll keeps the caller's flags beside its own WNOHANG. */
int sp_sched_wait_child_flags(int pid, int *status, int flags) {
  int r;
  if ((flags & WNOHANG) || !sp_sched_other_threads_live()) {
    do { r = (int)waitpid((pid_t)pid, status, flags); } while (r < 0 && errno == EINTR);
    return r;
  }
  { double back = 0.0002;
    for (;;) {
      do { r = (int)waitpid((pid_t)pid, status, WNOHANG | flags); } while (r < 0 && errno == EINTR);
      if (r != 0) return r;   /* reaped, or an error for the caller to report */
      sp_Thread_pass();
#ifdef SP_THREADS
      sp_sched_sleep(back);
#else
      { struct timespec rq; rq.tv_sec = 0; rq.tv_nsec = (long)(back * 1e9);
        while (nanosleep(&rq, &rq) == -1 && errno == EINTR) {} }
#endif
      if (back < 0.005) back *= 2.0;
    } }
}
static void reg_remove(sp_thread *t) {
  REG_LOCK();
  if (t->all_prev) t->all_prev->all_next = t->all_next;
  else if (g_all == t) g_all = t->all_next;
  if (t->all_next) t->all_next->all_prev = t->all_prev;
  t->all_prev = t->all_next = NULL;
  REG_UNLOCK();
}

/* GC: root every live green thread (and thus its fiber, stack roots, and
   pending result) so a fire-and-forget thread with no user reference is not
   collected mid-run. Chained ahead of whatever globals hook was installed. */
static void (*g_prev_globals_hook)(void) = NULL;
static sp_Fiber *g_main_root = NULL;   /* worker 0's root fiber: the main thread's */
/* The fiber a #raise aimed at t is queued on: a spawned thread's own, and for
   the main thread the root fiber it runs on. */
static sp_Fiber *sp_thread_inject_fiber(sp_thread *t) { return t == &g_main_thread ? g_main_root : t->fiber; }
static int sp_thread_inject_pending(sp_thread *t) {
  sp_Fiber *f = sp_thread_inject_fiber(t);
  return f && sp_fiber_inject_pending(f);
}
/* The same for the running thread, for a delivery inside a Fiber it resumed
   (sp_fiber_fire_inject_if_pending, lib/sp_fiber.c). */
sp_Fiber *sp_thread_inject_home(void) { return g_current ? sp_thread_inject_fiber(g_current) : NULL; }
static void sp_sched_globals_mark(void) {
  REG_LOCK();
  for (sp_thread *t = g_all; t; t = t->all_next) sp_gc_mark(t);
  REG_UNLOCK();
#ifdef SP_THREADS
  /* A timer entry holds its thread until the monitor drops it, stale or not:
     the monitor reads the thread's armed generation to tell. The monitor
     does not run while the world is stopped (sp_sysmon_main). */
  for (size_t i = 0; i < g_timer_heap_len; i++)
    if (g_timer_heap[i].thread != &g_main_thread) sp_gc_mark(g_timer_heap[i].thread);
  for (int w = 0; w < SP_MAX_WORKERS; w++) {
    sp_tm_stage *s = &g_tm_stage[w];
    if (!SP_ATOMIC_LOAD(&s->len, __ATOMIC_SEQ_CST)) continue;
    wl_lock(&s->wl);
    for (size_t i = 0; i < s->len; i++)
      if (s->ent[i].thread != &g_main_thread) sp_gc_mark(s->ent[i].thread);
    wl_unlock(&s->wl);
  }
#endif
  /* The main thread is a static struct, not a registry entry, so nothing
     above marks what hangs off it -- and its thread-local map
     (Thread.current[:k] = v on the main thread) is a GC object. Unmarked, a
     collection freed the map while g_main_thread.tls still pointed at it,
     and the next Thread#[]= wrote into freed memory. */
  if (g_main_thread.tls) sp_gc_mark(g_main_thread.tls);
  /* ...and so is its name: Thread#name= stores the String in the static
     struct, where sp_gc_wb cannot remember it, so a heap String (not a
     literal) was freed by the next collection and Thread#name read the
     freed slot. */
  sp_mark_rbval(g_main_thread.name);
  /* The main thread runs on worker 0's root fiber, which is thread-local, so
     sp_mark_fiber_root_storage reaches it only when worker 0 collects. Its
     storage (Fiber[:k] = v, Fiber.current.storage = h at top level) is marked
     here for a collection run by any other worker. */
  if (g_main_root) {
    if (g_main_root->storage) sp_gc_mark(g_main_root->storage);
    if (g_main_root->attrs) sp_gc_mark(g_main_root->attrs);
    /* and a #raise queued on it from another thread, until main takes it */
    if (g_main_root->inj_obj) sp_gc_mark(g_main_root->inj_obj);
    if (g_main_root->inj_msg) sp_mark_string(g_main_root->inj_msg);
  }
#ifdef SP_THREADS
  /* Mark each parked worker's published roots. Reaches the per-worker root fibers
     (idle/main workers) that are not on sp_fiber_list_head; green-thread fibers
     are also covered here (harmless re-mark) and via the suspended-fibers hook. */
  for (int i = 0; i < g_n_parked_fiber; i++) sp_fiber_mark_chain(g_parked_fiber[i]);   /* and the resumers waiting on it */
#endif
  if (g_prev_globals_hook) g_prev_globals_hook();
}

#ifdef SP_THREADS
static int sp_sched_start_workers(void);   /* defined after run_thread_once */
#endif

void sp_sched_init(void) {
  /* Called from main() before any fiber/thread op. Adopt this OS thread (worker
     0) as the main green thread: its native stack is the per-worker root fiber. */
  sp_fiber_worker_init();
  g_main_root = sp_fiber_worker_root();
  memset(&g_main_thread, 0, sizeof g_main_thread);
  g_main_thread.fiber = NULL;
  SP_TH_SET_STATE(&g_main_thread, SP_TH_RUNNING);
  g_main_thread.report_on_exception = 1;
  g_main_thread.arg = sp_box_nil();
  g_main_thread.retval = sp_box_nil();
  g_main_thread.name = sp_box_nil();
  g_current = &g_main_thread;
  g_prev_globals_hook = sp_gc_mark_globals_hook;
  sp_gc_mark_globals_hook = sp_sched_globals_mark;
#ifdef SP_THREADS
  sp_worker_id = 0;                          /* main is worker 0 */
  g_cnt_me = &g_cnt[0];
  g_wslot[0].tid = pthread_self();
  g_wslot[0].active = 1;
  /* Helpers + monitor are spawned lazily on the first Thread (sp_sched_ensure_workers),
     not here: a program that only uses Mutex/Queue/ConditionVariable/sleep for its
     structure -- but never spawns a second thread -- then runs entirely on main with
     no idle helper OS threads and no monitor. */
#endif
}

#ifdef SP_THREADS
/* Spawn the helper pool + monitor the first time a green thread is created. The
   first Thread ever always originates on main (no helper exists to run user code
   before this), so this runs single-threaded the one time it does work; later
   calls (a green thread spawning another) see the flag set and return. */
/* Drive the parallel sweep from inside sp_gc_collect, with the world already
   stopped and every other worker parked in sp_stw_park_locked. Waking them
   through g_stw_release is safe: their loop re-tests g_stw_active, so a
   wake that is not a release puts them back to sleep. */
/* ---- The concurrent sweep ----
   The mark runs under the barrier and the sweep does not. At the end of the
   mark the collector detaches every worker's young lists (objects; the
   SP_STR_YSUB string lists) and, on a full cycle, the old lists, replaces
   them with empty ones, and makes the detached lists a task list; the world
   resumes while it is swept. Who sweeps what: a worker's own young lists are
   swept by that worker, right after it is released and before it runs any
   program (sp_cs_owner_run), so the slots it frees are the ones it allocates
   from next and are still in its own cache from the walk (swept by another
   core they came back cold, and the mutators measured slower than under the
   stop-the-world sweep); the old lists, whose slots nobody is about to
   reuse, go to a small pool of sweeper threads. Whatever is unclaimed when
   the next collection stops the world is finished there by everyone who is
   parked (sp_sched_conc_wait, the first thing sp_gc_collect does), so the
   apply that follows sees a complete sweep. What is applied under the
   barrier is only what the mutators must not see half-done: the survivors
   spliced onto the old lists and the string budget retune. The pooled dead
   are pushed onto their pools by the sweep itself (see sp_gc_sweep_list);
   done at the barrier they cost it more than the sweep had shed.

   What makes the sweep safe beside the mutators: a mutator never reads a
   GC list link; the mark promoted every survivor before the world resumed
   (sp_gc_conc_promote), so the write barrier records stores into them and
   the sweeper never writes the flag word the barrier writes; a dead object is
   unreachable, so its finalizer races nothing; a string's mark byte is reset
   with a compare-and-swap (a freeze can land on the same byte); freed slab
   slots are pushed in per-chunk batches with a compare-and-swap that the
   owner's pop also uses; and the length caches were cleared when the workers
   parked, so no cache names a string the sweep frees. */
#define CS_OBJ 0
#define CS_OBJ_OLD 1
#define CS_STR_YOUNG 2
#define CS_STR_OLD 3
#define CS_CHUNKS 4     /* a slot's slab chunks, by their bitmaps (objects and strings, both generations) */
#define CS_TASK_MAX (SP_MAX_WORKERS * (3 + SP_STR_YSUB) + 1)
#define CS_SWEEPER_MAX 32
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cs_go = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_cs_done = PTHREAD_COND_INITIALIZER;
static int      g_cs_nsweepers = 0;
static unsigned g_cs_gen = 0;        /* bumped per sweep started; sweepers wait for a new one */
static int      g_cs_pending = 0;    /* a sweep is in flight */
static int      g_cs_str_sweep = 0, g_cs_str_major = 0;
static sp_sw_task g_cs_tasks[CS_TASK_MAX];
/* the detached lists (inputs) and the per-slot results */
static sp_gc_hdr  *g_cs_obj[SP_MAX_WORKERS];
static sp_gc_hdr  *g_cs_obj_old = NULL, *g_cs_obj_old_tail = NULL;
static sp_str_hdr *g_cs_str_young[SP_MAX_WORKERS][SP_STR_YSUB];
static sp_str_hdr *g_cs_str_old[SP_MAX_WORKERS];
static sp_gc_hdr  *g_cs_pro_head[SP_MAX_WORKERS], *g_cs_pro_tail[SP_MAX_WORKERS];
static sp_str_hdr *g_cs_sy_keep[SP_MAX_WORKERS][SP_STR_YSUB], *g_cs_sy_tail[SP_MAX_WORKERS][SP_STR_YSUB];
static size_t      g_cs_sy_moved[SP_MAX_WORKERS][SP_STR_YSUB];
static size_t      g_cs_so_freed[SP_MAX_WORKERS];
static unsigned long g_cs_task_us = 0;   /* SPINEL_GC_PHASES: sweeper time, summed */
static int      g_cs_hi = 1;         /* slots in use, plus the sweepers' own */
#define CS_SWEEPER_WID (SP_MAX_WORKERS - 1)   /* a sweeper's worker id: its own slot, should a finalizer allocate */

static double g_cs_t0 = 0;              /* SPINEL_GC_PHASES: when the sweep started */
static unsigned long g_cs_task_max_us = 0;
static void sp_cs_run_task(const sp_sw_task *t) {
  size_t dummy = 0;
  double t0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  switch (t->kind) {
    case CS_OBJ:
      sp_gc_sweep_list(&g_cs_obj[t->wid], 1, &g_cs_pro_head[t->wid], &g_cs_pro_tail[t->wid], &dummy);
      break;
    case CS_OBJ_OLD:
      sp_gc_sweep_old_list(&g_cs_obj_old, &dummy, &g_cs_obj_old_tail);
      break;
    case CS_CHUNKS:
      sp_gc_sweep_chunks(t->wid, g_cs_full);
      break;
    case CS_STR_YOUNG: {
      size_t held = 0;
      sp_str_sweep_young_list(&g_cs_str_young[t->wid][t->sub], &g_cs_sy_keep[t->wid][t->sub],
                              &g_cs_sy_tail[t->wid][t->sub], &g_cs_sy_moved[t->wid][t->sub], &held);
      break; }
    default:
      g_cs_so_freed[t->wid] = sp_str_sweep_old_list(&g_cs_str_old[t->wid]);
      break;
  }
  if (sp_gc_ph_on) {
    unsigned long d = (unsigned long)((sp_monotonic_now() - t0) * 1e6), m;
    do { m = SP_ATOMIC_LOAD(&g_cs_task_max_us, __ATOMIC_RELAXED); if (d <= m) break; }
    while (!SP_ATOMIC_CAS(&g_cs_task_max_us, &m, d, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  }
}
/* Who takes a task: a worker's own young lists (CS_OBJ, CS_STR_YOUNG of a
   live slot) are the owner's, so the freed slots come back warm to the core
   that allocates from them; the old lists and the sweepers' own slot are
   the sweeper threads'; under the barrier anyone takes what is left. */
static unsigned char g_cs_claimed[CS_TASK_MAX];
/* A young list is its owner's to sweep (warm in its cache, and it is the
   next to allocate from those slots). An idle owner is kicked at the release
   to do exactly that; one out in a blocking native call cannot be, so a
   sweeper thread takes its lists, or the next barrier would be spent
   finishing them. */
static int sp_cs_task_is_owned(const sp_sw_task *t) {
  return (t->kind == CS_OBJ || t->kind == CS_STR_YOUNG || t->kind == CS_CHUNKS) && t->wid != CS_SWEEPER_WID &&
         !(t->wid >= 0 && t->wid < SP_MAX_WORKERS && SP_ATOMIC_LOAD(&g_native_out[t->wid], __ATOMIC_RELAXED) == SP_OUT_NATIVE);
}
#define CS_RUN_SWEEPER 0
#define CS_RUN_OWNER   1
#define CS_RUN_ANY     2
static int sp_cs_run_tasks(int mode, int wid) {
  int ran = 0;
  double t0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  for (int i = 0; i < g_cs_ntasks; i++) {
    if (SP_ATOMIC_LOAD(&g_cs_claimed[i], __ATOMIC_RELAXED)) continue;
    const sp_sw_task *t = &g_cs_tasks[i];
    int owned = g_cs_owner_env && sp_cs_task_is_owned(t);
    if (mode == CS_RUN_SWEEPER && owned) continue;
    if (mode == CS_RUN_OWNER && (!owned || t->wid != wid)) continue;
    unsigned char z = 0;
    if (!SP_ATOMIC_CAS(&g_cs_claimed[i], &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
    SP_ATOMIC_FETCH_SUB(&g_cs_unclaimed, 1, __ATOMIC_RELAXED);
    sp_cs_run_task(t);
    SP_ATOMIC_STORE(&g_cs_claimed[i], 2, __ATOMIC_RELEASE);   /* done: the owner's release reads this */
    ran++;
  }
  if (sp_gc_ph_on && ran) SP_ATOMIC_FETCH_ADD(&g_cs_task_us, (unsigned long)((sp_monotonic_now() - t0) * 1e6), __ATOMIC_RELAXED);
  return ran;
}
/* May this worker hand its empty chunks back? Not while a sweeper thread is
   still in the chunk task of its slot (claimed, not done): that sweep clears
   bits of the very chunks the pool would carve for someone else. A slot with
   no task in this sweep, or whose task this worker ran itself, is settled. */
static int sp_cs_chunks_settled(int wid) {
  if (!g_cs_pending) return 1;
  for (int i = 0; i < g_cs_ntasks; i++)
    if (g_cs_tasks[i].kind == CS_CHUNKS && g_cs_tasks[i].wid == wid)
      return SP_ATOMIC_LOAD(&g_cs_claimed[i], __ATOMIC_ACQUIRE) == 2;
  return 1;
}
/* Leaving the task list: count what was run, and count ourselves out.
   The collector reuses the task array for the next sweep only once nobody is
   scanning it (g_cs_running), not merely once every task is done: a thread
   still walking the array for something to claim would otherwise read the
   next sweep's entries as they are written. */
static void sp_cs_finish(int ran) {
  pthread_mutex_lock(&g_cs_lock);
  /* release: the collector may find both counters at rest with an acquire
     load and proceed without the lock, and it then reads what the tasks wrote */
  int fin = SP_ATOMIC_ADD_FETCH(&g_cs_finished, ran, __ATOMIC_RELEASE);
  int running = SP_ATOMIC_SUB_FETCH(&g_cs_running, 1, __ATOMIC_RELEASE);
  if (fin >= g_cs_ntasks && running == 0) {
    if (sp_gc_ph_on) sp_gc_ph_conc_wall += sp_monotonic_now() - g_cs_t0;
    pthread_cond_broadcast(&g_cs_done);
  }
  pthread_mutex_unlock(&g_cs_lock);
}
/* Entering the task list: only a sweep with a task still unclaimed. A
   thread that saw work to do and reaches here late -- a sweeper thread the
   OS woke after the others finished, a parked helper descheduled between
   its look and this lock -- may find the sweep over, and the collector, who
   read g_cs_running at zero, already writing the task array and the claim
   flags for the next one. Counting itself in then would walk them while
   they are written: ThreadSanitizer reports it (sp_cs_run_tasks against
   sp_sched_conc_start and its memset), and on a weakly ordered CPU the walk
   can claim a cleared flag and still read the previous sweep's task.
   Under g_cs_lock the count is exact enough: a claim whose task finished
   was counted out before its sp_cs_finish took this lock, and a claimer
   still to finish takes it after us and so sees us running, which keeps
   the collector waiting for us. PRE: g_cs_lock held. */
static int sp_cs_join_locked(void) {
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) == 0) return 0;
  SP_ATOMIC_FETCH_ADD(&g_cs_running, 1, __ATOMIC_RELAXED);
  return 1;
}
static int sp_cs_enter(void) { pthread_mutex_lock(&g_cs_lock); int in = sp_cs_join_locked(); pthread_mutex_unlock(&g_cs_lock); return in; }
/* The owner's share, run by the released worker with the world running. */
static void sp_cs_owner_run(int wid) {
  int was = sp_gc_in_sweeper;
  if (!sp_cs_enter()) return;
  sp_gc_in_sweeper = 1;
  int ran = sp_cs_run_tasks(CS_RUN_OWNER, wid);
  sp_gc_in_sweeper = was;
  sp_cs_finish(ran);
}
/* A parked worker (or the collector) finishing the sweep under the barrier:
   it sweeps like a sweeper thread does, with the byte accounting off, since
   the collector recounts the bytes the mark saw. */
static void sp_cs_help_run(void) {
  int was = sp_gc_in_sweeper;
  if (!sp_cs_enter()) return;
  sp_gc_in_sweeper = 1;
  int ran = sp_cs_run_tasks(CS_RUN_ANY, -1);
  sp_gc_in_sweeper = was;
  sp_cs_finish(ran);
}
static void *sp_cs_sweeper_main(void *arg) {
  (void)arg;
  sigset_t blk; sigemptyset(&blk); sigaddset(&blk, g_preempt_sig);
  pthread_sigmask(SIG_BLOCK, &blk, NULL);
  sp_gc_in_sweeper = 1;
  sp_worker_id = CS_SWEEPER_WID;
#ifdef __linux__
  prctl(PR_SET_NAME, "sp-sweeper", 0, 0, 0);   /* pthread_setname_np needs _GNU_SOURCE; this does not (#4469) */
#endif
  unsigned seen = 0;
  for (;;) {
    pthread_mutex_lock(&g_cs_lock);
    /* g_shutdown is set under the sched lock at drain, which this thread
       does not hold: read it atomically, as the trim thread does */
    while (g_cs_gen == seen && !SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)) pthread_cond_wait(&g_cs_go, &g_cs_lock);
    if (SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)) { pthread_mutex_unlock(&g_cs_lock); break; }
    seen = g_cs_gen;
    int in = sp_cs_join_locked();
    pthread_mutex_unlock(&g_cs_lock);
    if (in) sp_cs_finish(sp_cs_run_tasks(CS_RUN_SWEEPER, -1));
  }
  return NULL;
}
/* Under the barrier, at the end of the mark: detach and hand off. */
static void sp_sched_conc_start(int full, int str_sweep, int str_major) {
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS - 1) n = SP_MAX_WORKERS - 1;
  if (n > g_cs_hi) g_cs_hi = n;
  int nt = 0;
  g_cs_full = full; g_cs_str_sweep = str_sweep; g_cs_str_major = str_major;
  /* The collector's own length cache: every parked worker cleared its own at
     the park; this thread did not park, and it may name strings the sweep
     is about to free, whose addresses a later allocation reuses. */
  sp_str_lcache_clear();
  /* every slot ever used, plus the sweepers' own: a finalizer that
     allocated would have pushed onto CS_SWEEPER_WID's lists */
  int slots[SP_MAX_WORKERS]; int ns = 0;
  for (int i = 0; i < g_cs_hi; i++) slots[ns++] = i;
  slots[ns++] = CS_SWEEPER_WID;
  for (int k = 0; k < ns; k++) {
    int i = slots[k];
    g_cs_pro_head[i] = g_cs_pro_tail[i] = NULL;
    g_cs_obj[i] = sp_gc_wslot[i].young; sp_gc_wslot[i].young = NULL;
    if (g_cs_obj[i]) g_cs_tasks[nt++] = (sp_sw_task){ CS_OBJ, (short)i, 0 };
    /* the slot's chunks: the young slab generation every cycle, the old one
       at a full, strings with objects -- the string gate below governs only
       the lists of the strings too large for the slab (and their retune) */
    g_cs_tasks[nt++] = (sp_sw_task){ CS_CHUNKS, (short)i, 0 };
    if (str_sweep) {
      g_cs_so_freed[i] = 0;
      if (str_major) {
        g_cs_str_old[i] = sp_str_wslot[i].old; sp_str_wslot[i].old = NULL;
        if (g_cs_str_old[i]) g_cs_tasks[nt++] = (sp_sw_task){ CS_STR_OLD, (short)i, 0 };
      }
      for (int sub = 0; sub < SP_STR_YSUB; sub++) {
        g_cs_sy_keep[i][sub] = g_cs_sy_tail[i][sub] = NULL; g_cs_sy_moved[i][sub] = 0;
        g_cs_str_young[i][sub] = sp_str_wslot[i].young[sub]; sp_str_wslot[i].young[sub] = NULL;
        if (g_cs_str_young[i][sub]) g_cs_tasks[nt++] = (sp_sw_task){ CS_STR_YOUNG, (short)i, (short)sub };
      }
      /* the young generation left with its lists; the trigger counts what
         the slot holds from here */
      SP_GC_CTR_SET(sp_str_wslot[i].young_bytes, 0);
      sp_str_wslot[i].ask_at = 0;
    }
  }
  if (full) {
    g_cs_obj_old = sp_gc_old_detach();
    if (g_cs_obj_old) g_cs_tasks[nt++] = (sp_sw_task){ CS_OBJ_OLD, 0, 0 };
  }
  pthread_mutex_lock(&g_cs_lock);
  if (sp_gc_ph_on) g_cs_t0 = sp_monotonic_now();
  g_cs_ntasks = nt; SP_ATOMIC_STORE(&g_cs_finished, 0, __ATOMIC_RELAXED);
  memset(g_cs_claimed, 0, (size_t)nt);
  SP_ATOMIC_STORE(&g_cs_unclaimed, nt, __ATOMIC_RELEASE);
  g_cs_pending = 1;
  g_cs_gen++;
  pthread_cond_broadcast(&g_cs_go);
  pthread_mutex_unlock(&g_cs_lock);
}
/* Under the barrier, at the start of the next collection (or the end of an
   explicit one): join the sweepers and apply what they produced. */
static void sp_sched_conc_wait(void) {
  if (!g_cs_pending) return;
  if (SP_ATOMIC_LOAD(&g_cs_finished, __ATOMIC_ACQUIRE) < g_cs_ntasks || SP_ATOMIC_LOAD(&g_cs_running, __ATOMIC_ACQUIRE) > 0) {
    /* Not done yet: the world is stopped, so every parked worker is idle.
       Hand them the unclaimed tasks (thirty hands finish in a fraction of
       what eight sweepers need), take some ourselves, then join. */
    double t0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
    SCHED_LOCK();
    g_cs_help = 1;
    pthread_cond_broadcast(&g_stw_release);
    SCHED_UNLOCK();
    sp_cs_help_run();
    pthread_mutex_lock(&g_cs_lock);
    while (SP_ATOMIC_LOAD(&g_cs_finished, __ATOMIC_RELAXED) < g_cs_ntasks || SP_ATOMIC_LOAD(&g_cs_running, __ATOMIC_RELAXED) > 0) pthread_cond_wait(&g_cs_done, &g_cs_lock);
    pthread_mutex_unlock(&g_cs_lock);
    SCHED_LOCK();
    g_cs_help = 0;
    SCHED_UNLOCK();
    if (sp_gc_ph_on) { sp_gc_ph_conc_wait += sp_monotonic_now() - t0; sp_gc_ph_conc_waits++; }
  }
  if (sp_gc_ph_on) { sp_gc_ph_task_sum += (double)g_cs_task_us * 1e-6; g_cs_task_us = 0;
                     sp_gc_ph_slot_max += (double)g_cs_task_max_us * 1e-6; g_cs_task_max_us = 0; }
  double at0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  /* the swept old list comes back first, then the survivors go in front of it */
  if (g_cs_full) { sp_gc_old_attach(g_cs_obj_old, g_cs_obj_old_tail); g_cs_obj_old = g_cs_obj_old_tail = NULL; }
  int slots[SP_MAX_WORKERS]; int ns = 0;
  for (int i = 0; i < g_cs_hi; i++) slots[ns++] = i;
  slots[ns++] = CS_SWEEPER_WID;
  for (int k = 0; k < ns; k++) {
    int i = slots[k];
    /* bytes: the mark counted them into the old total already */
    sp_gc_promote_slot(g_cs_pro_head[i], g_cs_pro_tail[i], 0);
    g_cs_pro_head[i] = g_cs_pro_tail[i] = NULL;
  }
  if (sp_gc_ph_on) { double t = sp_monotonic_now(); sp_gc_ph_apply_obj += t - at0; at0 = t; }
  if (g_cs_str_sweep) {
    size_t promoted = 0;
    size_t young_now = sp_str_bytes_total();
    for (int k = 0; k < ns; k++) {
      int i = slots[k];
      if (g_cs_str_major) {
        /* the old list was detached whole; what the sweep left of it is
           the old list again, and the young survivors go in front */
        sp_str_wslot[i].old = g_cs_str_old[i]; g_cs_str_old[i] = NULL;
        sp_str_wslot[i].old_bytes = sp_str_wslot[i].old_bytes > g_cs_so_freed[i] ? sp_str_wslot[i].old_bytes - g_cs_so_freed[i] : 0;
      }
      for (int sub = 0; sub < SP_STR_YSUB; sub++)
        sp_str_sweep_young_done(i, g_cs_sy_keep[i][sub], g_cs_sy_tail[i][sub], g_cs_sy_moved[i][sub], 0, &promoted);
    }
    sp_str_sweep_end_excluding(g_cs_str_major, promoted, young_now);
  }
  if (sp_gc_ph_on) { double t = sp_monotonic_now(); sp_gc_ph_apply_str += t - at0; at0 = t; }
  /* the owners released their own chunks beside the program (sp_stw_park_locked's
     exit); what is left for the barrier is the slots no active worker owns */
  if (g_cs_full) sp_slab_release_from(sp_active_workers);
  if (sp_gc_ph_on) sp_gc_ph_apply_release += sp_monotonic_now() - at0;
  g_cs_pending = 0;
}
/* Under the barrier, at the end of the root walk: lend the parked workers
   to the drain, drain with them, and join them before the mark is declared
   done (a late helper that found nothing must still be out of the drain). */
static void sp_sched_par_mark(void) {
  sp_gc_mark_par_begin();
  SCHED_LOCK();
  g_mk_gen++; g_mk_joined = 0; g_mk_go = 1;
  /* as many wake-ups as there are seats: a broadcast had thirty parked
     workers take the scheduler lock in turn to find no seat, and that
     procession cost more than the drain */
  for (int i = 0; i < g_mk_max; i++) pthread_cond_signal(&g_stw_release);
  SCHED_UNLOCK();
  double t0 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  sp_gc_mark_par_run();
  double t1 = sp_gc_ph_on ? sp_monotonic_now() : 0;
  SCHED_LOCK();
  g_mk_go = 0;
  while (g_mk_active > 0) pthread_cond_wait(&g_mk_cv, &g_sched_lock);
  if (sp_gc_ph_on) { double t2 = sp_monotonic_now(); sp_gc_ph_mk_drain += t1 - t0; sp_gc_ph_mk_join += t2 - t1; }
  if (sp_gc_ph_on) { sp_gc_ph_mk_helpers += (unsigned long long)g_mk_joined; sp_gc_ph_mk_drains++; }
  SCHED_UNLOCK();
}
static int sp_available_cpus(void);   /* defined below; the CPUs this process may run on */
static void sp_cs_start_sweepers(void) {
  { const char *o = getenv("SPINEL_GC_OWNER"); g_cs_owner_env = !(o && *o == '0'); }
  const char *e = getenv("SPINEL_GC_SWEEPERS");
  int want = e && *e ? atoi(e) : 0;
  if (want <= 0) {
    /* On a single CPU nothing sweeps beside the program: a sweeper thread
       runs only by taking that CPU from the worker, so every collection
       costs the wake-ups and the switches there and back for a sweep the
       collector does in the same time itself. No sweeper starts then, the
       hooks below stay unset, and the collector sweeps under the barrier as
       it does under SPINEL_GC_CONC=0. A count asked for by name is still
       started. */
    if (sp_available_cpus() < 2) return;
    want = g_worker_cap > 0 ? g_worker_cap : 1; if (want > 8) want = 8;
  }
  if (want > CS_SWEEPER_MAX) want = CS_SWEEPER_MAX;
  pthread_attr_t at; pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1024 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < want; i++) {
    pthread_t t;
    if (pthread_create(&t, &at, sp_cs_sweeper_main, NULL) == 0) g_cs_nsweepers++;
  }
  pthread_attr_destroy(&at);
  if (g_cs_nsweepers > 0) {
    sp_gc_conc_sweep_hook = sp_sched_conc_start;
    sp_gc_conc_wait_hook = sp_sched_conc_wait;
  }
}

static int    g_sw_hi = 1;   /* largest pool seen; bounds the per-collection loops */
static void sp_sched_par_sweep(void) {
  int n = sp_active_workers; if (n < 1) n = 1; if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  /* Only the live slots: SP_MAX_WORKERS is 256 and this runs on every
     collection, so clearing the whole array cost more than the sweep saved.
     g_sw_hi is the largest pool ever seen, which bounds the orphan scan below
     without walking 256 empty slots each time. */
  if (n > g_sw_hi) g_sw_hi = n;
  /* The string gate is one decision over the whole heap, so make it here --
     once, before anyone starts -- and let each worker apply it to its own
     slot. Taking it per worker would let them disagree about `major`. */
  g_str_major = 0;
  g_str_sweep = sp_str_sweep_begin(&g_str_major);
  /* Same gate the serial sweep applies: on a minor cycle the old string list
     holds exactly the strings the mark could not reach, so sweeping it frees
     live ones. Without this a threaded program corrupted a Hash the moment a
     minor cycle landed on a string-heap trigger. */
  if (sp_gc_str_minor_only) g_str_major = 0;
  sp_str_par_done = 1;   /* the collector's serial pass must not repeat this */
  /* Every slot up to the largest pool seen is on the list: a slot no live
     worker owns -- past the current pool, or a worker that exited -- still
     has to be swept or its lists leak, and its string lists count too. The
     heavy tasks go first so the tail of the phase is short ones. */
  memset(g_str_promoted, 0, (size_t)g_sw_hi * sizeof g_str_promoted[0]);
  memset(g_sw_head, 0, (size_t)g_sw_hi * sizeof g_sw_head[0]);
  memset(g_sw_tail, 0, (size_t)g_sw_hi * sizeof g_sw_tail[0]);
  memset(g_sw_bytes, 0, (size_t)g_sw_hi * sizeof g_sw_bytes[0]);
  int nt = 0;
  if (g_str_sweep) {
    if (g_str_major)
      for (int i = 0; i < g_sw_hi; i++)
        if (sp_str_wslot[i].old) g_sw_tasks[nt++] = (sp_sw_task){ SW_STR_OLD, (short)i, 0 };
    for (int i = 0; i < g_sw_hi; i++)
      for (int sub = 0; sub < SP_STR_YSUB; sub++) {
        g_sy_keep[i][sub] = g_sy_tail[i][sub] = NULL; g_sy_moved[i][sub] = g_sy_held[i][sub] = 0;
        if (sp_str_wslot[i].young[sub]) g_sw_tasks[nt++] = (sp_sw_task){ SW_STR_YOUNG, (short)i, (short)sub };
      }
  }
  for (int i = 0; i < g_sw_hi; i++)
    if (sp_gc_wslot[i].young) g_sw_tasks[nt++] = (sp_sw_task){ SW_OBJ, (short)i, 0 };
  /* every slot's slab chunks, the slots no worker runs included: a chunk is
     swept exactly once a cycle, and a second pass over one at a full cycle
     would read its cleared marks as "nothing reached" */
  for (int i = 0; i < SP_MAX_WORKERS; i++)
    g_sw_tasks[nt++] = (sp_sw_task){ SW_CHUNKS, (short)i, 0 };
  /* The collector's own length cache: every parked worker cleared its own
     at the park, and until now the collector dropped its entries one by one
     as it swept its own strings. Its strings may now be swept by any worker,
     which drops them from THAT worker's (empty) cache, so the collector's
     entries would name freed strings. Clear it whole, like a park does. */
  sp_str_lcache_clear();
  SCHED_LOCK();
  g_sw_ntasks = nt;
  SP_ATOMIC_STORE(&g_sw_next, 0, __ATOMIC_RELEASE);
  g_sw_done = 0;
  g_sw_slot_max_us = 0;
  g_sweep_go = 1;
  pthread_cond_broadcast(&g_stw_release);
  SCHED_UNLOCK();
  int ran = sp_sweep_run_tasks();
  SCHED_LOCK();
  g_sw_done += ran;
  while (g_sw_done < nt) pthread_cond_wait(&g_sweep_cv, &g_sched_lock);
  g_sweep_go = 0;
  SCHED_UNLOCK();
  for (int i = 0; i < g_sw_hi; i++)
    sp_gc_promote_slot(g_sw_head[i], g_sw_tail[i], g_sw_bytes[i]);
  if (sp_gc_ph_on) {
    sp_gc_ph_slot_max += (double)g_sw_slot_max_us * 1e-6;
    sp_gc_ph_task_sum += (double)g_sw_task_sum_us * 1e-6;
    sp_gc_ph_task_obj += (double)g_sw_task_max_kind[SW_OBJ] * 1e-6;
    sp_gc_ph_task_sold += (double)g_sw_task_max_kind[SW_STR_OLD] * 1e-6;
    sp_gc_ph_task_syoung += (double)g_sw_task_max_kind[SW_STR_YOUNG] * 1e-6;
    g_sw_task_sum_us = 0; memset(g_sw_task_max_kind, 0, sizeof g_sw_task_max_kind);
  }
  if (g_str_sweep) {
    size_t promoted = 0;
    for (int i = 0; i < g_sw_hi; i++)
      for (int sub = 0; sub < SP_STR_YSUB; sub++)
        sp_str_sweep_young_done(i, g_sy_keep[i][sub], g_sy_tail[i][sub], g_sy_moved[i][sub], g_sy_held[i][sub], &promoted);
    sp_str_sweep_end(g_str_major, promoted);
    g_str_sweep = 0;
  }
}

/* The trimmer: malloc_trim beside the running program rather than under
   stop-the-world (see the full-cycle tail of sp_gc_collect). It wakes once a
   second and trims when a full cycle has asked since the last one. */
#if defined(__GLIBC__)
#include <malloc.h>
/* The trimmer sleeps in short steps and trims as soon as a full cycle has
   asked (the cadence, once a second, is the requester's): a trim a second
   after the request found the arena mid-cycle, with the freed memory of the
   last sweep only partly in it, and the process kept 260 MB more resident
   than the inline trim used to leave. Trimming promptly after the request,
   which follows the sweep, hands back what the sweep freed. */
static void *sp_trim_thread_main(void *arg) {
  (void)arg;
  sigset_t blk; sigemptyset(&blk); sigaddset(&blk, g_preempt_sig);
  pthread_sigmask(SIG_BLOCK, &blk, NULL);
  for (;;) {
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    if (SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)) break;   /* set under the sched lock at drain; read here without it */
    if (SP_ATOMIC_EXCHANGE(&sp_gc_trim_wanted, 0, __ATOMIC_ACQ_REL)) malloc_trim(0);
  }
  return NULL;
}
static void sp_trim_thread_start(void) {
  pthread_t t;
  pthread_attr_t at; pthread_attr_init(&at);
  /* 1 MB, like the sweepers: a thread's stack also carries the runtime's
     static TLS, and the mark stack there alone is half a megabyte, so the
     256 KB this asked for was refused by pthread_create (EINVAL). The
     trimmer then never started, and every full cycle's malloc_trim ran
     inline under the barrier -- 40 ms each, the whole reason the thread
     exists -- with nothing saying so. */
  pthread_attr_setstacksize(&at, 1024 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&t, &at, sp_trim_thread_main, NULL);
  if (rc == 0) sp_gc_trimmer_on = 1;
  else if (sp_gc_ph_on) fprintf(stderr, "[gcph] trimmer thread did not start: %s\n", strerror(rc));
  pthread_attr_destroy(&at);
}
#else
static void sp_trim_thread_start(void) {}
#endif

static int sp_worker_count(void);   /* defined below; the pool size */
static int sp_sched_ensure_workers(void) {
  if (g_workers_started) return 1;
  sp_alloc_stress_init();   /* set the stress flags once here, before any helper reads them */
  sp_alloc_worker_tune(sp_worker_count());  /* and size the GC budget for the pool */
  if (!sp_sched_start_workers()) return 0;
  g_workers_started = 1;
  /* Hand parked workers their own slots from here on. Workers spawn lazily, so
     the pool may still be one at this point; the driver itself falls back to
     the serial sweep whenever there is nobody parked to help. */
  sp_gc_par_sweep_hook = sp_sched_par_sweep;
  /* Seats for the parallel mark. Four helpers took a server's drain from
     2.0 ms to 0.7 ms; eight did no better and sixteen were slower, since
     every helper is a parked worker woken from a futex and the wake-ups
     and the chunk list's lock are the drain's fixed cost. */
  { const char *e = getenv("SPINEL_GC_MARKERS"); int m = e && *e ? atoi(e) : 0;
    if (m <= 0) { m = g_worker_cap > 0 ? g_worker_cap : 1; if (m > 4) m = 4; }
    g_mk_max = m; sp_gc_mark_par_markers(m + 1); }
  sp_gc_hdr_flags_check();
  sp_gc_par_mark_hook = sp_sched_par_mark;
  sp_trim_thread_start();
  sp_cs_start_sweepers();
  return 1;
}
#endif

/* CRuby's two-line report: the thread's own #inspect, then the same tail an
   uncaught exception prints. The old one-line form named the thread by its
   small serial, which says nothing about WHICH thread in a program that
   spawns several -- #inspect carries the spawn site. sp_Thread_inspect
   allocates, and the caller has already dropped t from the registry, so root
   t across it. */
static void sp_thread_report(sp_thread *t) {
  SP_GC_ROOT(t);
  const char *cls = t->exc_cls ? t->exc_cls : "Exception";
  const char *msg = t->exc_msg ? t->exc_msg : "";
  SP_GC_ROOT_STR(msg);
  const char *ins = sp_Thread_inspect(t);
  fprintf(stderr, "%s terminated with exception (report_on_exception is true):\n", ins);
  const char *shown = sp_exc_cls_display(cls);   /* an unnamed Class.new class takes no suffix */
  if (sp_exc_cls_unnamed(shown)) fprintf(stderr, "%s\n", (msg && *msg) ? msg : shown);
  else fprintf(stderr, "%s (%s)\n", (msg && *msg) ? msg : shown, shown);
}

/* Park/wake primitives (defined below; used by join here). */
static void       sp_sched_block(sp_thread **waitlist, int defer_inject);
static sp_thread *sp_sched_wake_one(sp_thread **waitlist);
static void       sp_sched_block_wl(sp_thread **waitlist, sp_wlock *wl, int defer_inject, sp_mutex *release);
static int        sched_main_leave_deadlocked_wait(sp_thread *self);
#ifdef SP_THREADS
static int        sp_sched_block_timeout(sp_thread **waitlist, double deadline, sp_mutex *mutex, sp_wlock *wl, int (*no_wait)(void *), void *no_wait_arg);
#endif

/* A finished thread's parked joiners become runnable again, main among them
   if it joins t (it waits in its pump, enlisted here). The list is under t's
   own wait lock (join_wl), and t is DEAD before it is taken, so a joiner that
   looks under that lock either finds t dead or is on the list taken here. They
   are collected under the lock and woken after it: a woken thread can run and
   park again at once, rewriting its links. No joiner enlists once t is dead,
   so the list only shrinks. */
static void sp_thread_wake_joiners(sp_thread *t) {
  for (;;) {
    sp_thread *ts[64]; int n = 0;
    wl_lock(&t->join_wl);
    for (sp_thread *w; n < 64 && (w = wl_take_one(&t->joiners)) != NULL; ) ts[n++] = w;
    wl_unlock(&t->join_wl);
    for (int i = 0; i < n; i++) sched_wake_unlinked(ts[i]);
    if (n < 64) return;
  }
}

/* Run runnable green threads until `target` is DEAD, until the main thread is
   woken back to RUNNABLE (it had blocked on a primitive), or until the run queue
   drains. Runs on the root fiber (the main thread). */
/* Run thread t for one timeslice: transfer into its fiber until it yields back
   (parked on a wait-list, or re-queued itself via Thread.pass) or its body
   returns. On termination, publish the result/exception and wake joiners. */
/* Quiescence: nothing left to run anywhere. A main thread parked in its pump
   (#join / drain) waits on exactly this, so the worker that empties the last of
   the work wakes it. Counting RUNNING threads (not idle workers) makes this robust:
   a worker that merely wakes spuriously and re-idles never touches the counts,
   so it cannot momentarily perturb the predicate the way an idle count would.
   (#8702) It is asked by the last helper to go idle (sp_out_enter_locked). */
static void sp_sched_signal_if_quiescent(void) {
  /* Only main is waiting on this. Quiescent means there is nothing for a
     helper to find, so waking the helpers here was pure cost -- and in a
     park-heavy program (every green thread blocked on I/O between hops) it
     fired on every hop, which is most of what adding workers cost (#4305). */
  if (counts_quiet()) SCHED_WAKE_MAIN();   /* none running, none runnable */
}

/* The fiber a green thread switches back to when it yields, blocks or is
   preempted: the one that ran it on this worker (run_thread_once). That is
   the worker's root fiber, except when the main thread runs a green thread
   from inside a Fiber it resumed (a Thread.pass sweep, or the pump): the root
   is then suspended in that #resume, its context saved in the fiber's
   caller_ctx rather than its own, so a switch to it lands on a context that
   was never saved. run_thread_once never nests on a worker, so it sets the
   home and clears it after. */
static SP_TLS sp_Fiber *g_sched_home = NULL;
static sp_Fiber *sp_sched_home(void) { return g_sched_home ? g_sched_home : sp_fiber_worker_root(); }
sp_Fiber *sp_sched_home_fiber(void) { return sp_sched_home(); }
/* A green thread yields, blocks or is preempted: back to the fiber that ran
   it. Inside a Fiber it resumed, that fiber is where it stops, so note it for
   run_thread_once to switch back to. */
static void sp_sched_switch_out(void) {
  sp_thread *self = g_current;
  if (self && sp_fiber_current != self->fiber) self->at = sp_fiber_current;
  sp_fiber_sched_switch(sp_sched_home());
}

static void run_thread_once(sp_thread *t) { sp_gc_wb((void*)t);   /* PRE/POST: sched lock held, or not held from the worker's fast path */
  int locked = g_sched_held;
  if (sched_lat_enabled() && t->readied_at > 0) {
    double d = (sp_monotonic_now() - t->readied_at) * 1e6;   /* us */
    int k = 0; while (k < 31 && d >= (double)(2u << k)) k++;
    g_sched_lat_hist[k]++; g_sched_lat_n++;
    if (d > g_sched_lat_max) g_sched_lat_max = d;
    t->readied_at = 0;
    int len = 0; for (sp_thread *w = g_lrq[sp_worker_id].head; w; w = w->rq_next) len++;
    if (len > g_lrq_len_max) g_lrq_len_max = len;
  }
  sp_thread *saved = g_current;
  g_current = t;
  if (g_run_taken) { g_run_taken = 0; RUNNABLE_TO_RUNNING(); }   /* taken for this run: runq_take */
  else RUNNING_INC();
  SP_TH_SET_STATE(t, SP_TH_RUNNING);
  SP_ATOMIC_STORE(&t->park, SP_PK_RUN, __ATOMIC_SEQ_CST);   /* on-cpu now */
  if (t->home_wid < 0) {   /* pin to this worker (TLS affinity) */
    if (!locked) SCHED_LOCK();
    t->home_wid = (short)sp_worker_id;
#ifdef SP_THREADS
    SP_ATOMIC_FETCH_ADD(&g_wslot[sp_worker_id].pinned, 1, __ATOMIC_RELAXED);
#endif
    if (!locked) SCHED_UNLOCK();
  }
#ifdef SP_THREADS
  /* Publish to the monitor that this worker is now running t, and when -- it uses
     this to enforce the timeslice. Nudge the monitor if it is idle so it starts
     ticking. */
  wslot_set_since(sp_worker_id, sp_monotonic_now());
  /* sequentially consistent, as is the read of g_sysmon_idle below: see the
     monitor's untimed wait (sp_sysmon_main) for why the pair cannot both miss */
  SP_ATOMIC_STORE(&g_wslot[sp_worker_id].cur, t, __ATOMIC_SEQ_CST);
  /* A monitor with nothing to time waits untimed: signal it under the lock its
     condvar uses, so it starts ticking. One in a timed wait that comes round
     within a quantum sees this worker busy then, and the preemption still
     lands by since + quantum + tick; nudging it here as well cost every
     thread start the scheduler lock (condvar) or a write to its pipe (poll).
     One that waits longer (nothing ran lately: SP_SYSMON_WARM) is signalled
     like an idle one -- under the lock for its condvar, which it holds from
     publishing g_sysmon_wake_at until its wait releases it. */
  double mon_at = sysmon_wake_at();
  if (SP_ATOMIC_LOAD(&g_sysmon_idle, __ATOMIC_SEQ_CST) == 1 ||
      (mon_at != 0.0 && mon_at > wslot_since(sp_worker_id) + SP_PREEMPT_QUANTUM)) {
    if (!locked) SCHED_LOCK();
    sp_sysmon_wake();
    if (!locked) SCHED_UNLOCK();
  }
#endif
  int raised = 0;
  const char *ec = NULL, *em = NULL;
  void *eo = NULL;
  /* On the body's first entry the block's param reads the fiber's resumed
     value, so hand it Thread.new's argument then; later resumes pass nil. */
  sp_RbVal in = (t->fiber->state == 0) ? t->arg : sp_box_nil();
  /* Run the green thread with the lock dropped: it executes Ruby (and may park
     on, or wake, other threads, which re-take the lock themselves). */
  g_sched_home = sp_fiber_current;
  sp_Fiber *at = t->at;   /* it stopped inside a Fiber it resumed */
  t->at = NULL;
  if (locked) SCHED_UNLOCK();
  sp_Fiber_transfer_catch(t->fiber, at, in, &raised, &ec, &em, &eo);
  if (locked) SCHED_LOCK();
  g_sched_home = NULL;
  g_current = saved;
#ifdef SP_THREADS
  SP_ATOMIC_STORE(&g_wslot[sp_worker_id].cur, NULL, __ATOMIC_SEQ_CST);   /* no longer timing t on this worker */
  /* If the monitor flagged t but it yielded/blocked/died before reaching a poll,
     retire the request here so the flag does not stay stuck set. Sequentially
     consistent, as the store above: see sched_preempt. */
  if (SP_ATOMIC_LOAD(&t->preempt_request, __ATOMIC_SEQ_CST)) {
    if (!locked) SCHED_LOCK();
    if (SP_ATOMIC_LOAD(&t->preempt_request, __ATOMIC_ACQUIRE)) { SP_ATOMIC_STORE(&t->preempt_request, 0, __ATOMIC_RELEASE); g_npreempt--; sp_recompute_safepoint_flag(); }
    if (!locked) SCHED_UNLOCK();
  }
#endif
  if (t->fiber->state == 3) {   /* the body returned (terminated) */
    /* death: the registry, the joiners and the pinned count each have a lock
       or an atomic of their own (#8702), so it takes the scheduler lock only
       to wake a main that joins t, or that waits for the last thread */
    t->retval = t->fiber->yielded_value;
#ifdef SP_THREADS
    if (t->home_wid >= 0) {
      int np = SP_ATOMIC_LOAD(&g_wslot[t->home_wid].pinned, __ATOMIC_RELAXED);
      while (np > 0 && !SP_ATOMIC_CAS(&g_wslot[t->home_wid].pinned, &np, np - 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }
#endif
    int do_report = 0;
    if (raised) {
      t->has_exc = 1; t->exc_cls = ec; t->exc_msg = em; t->exc_obj = eo;
      /* A SystemExit is not a thread dying badly, it is the program being
         asked to end: CRuby reports every other class here and stays silent
         for this one, then lets the exception surface at join and terminate
         with its status. Reporting it printed a scary line for an ordinary
         `exit` inside a thread. */
      int is_exit = ec && sp_exc_cls_matches(ec, "SystemExit");
      do_report = t->report_on_exception && !is_exit;
      /* ... and it ends the program from any wait main is in, ensures and
         at_exit hooks included, as CRuby raises it in the main thread. Ahead
         of the joiners' wake, so a main joining t raises it once. */
      if (is_exit) {
        if (!locked) SCHED_LOCK();
        sp_thread_raise_main_locked(ec, em, eo);
        if (!locked) SCHED_UNLOCK();
      }
    }
    /* DEAD goes up after retval and the exception fields, with release:
       #status and #inspect read has_exc once they see it, without the lock. */
    SP_ATOMIC_STORE(&t->state, SP_TH_DEAD, __ATOMIC_RELEASE);
    /* Record t AFTER the stores above, not only on the way into this function.
       The barrier at the top has been consumed by then: the transfer runs the
       thread's body, which allocates, and any collection in there clears every
       old object's dirty bit and empties the remembered set. retval and the
       exception fields are all allocated by that body, so an old t was left
       holding young values nothing had recorded -- and a minor mark does not
       walk the old list. Thread#value read a recycled string this way.

       A barrier before a call that can collect only covers the stores before
       the call; gc_wb_cells makes the same choice on the codegen side, and
       says why in its own comment. */
    sp_gc_wb((void *)t);
    sp_fiber_release_stack(t->fiber);   /* off its stack for good: the next thread's (sp_fiber.c) */
    sp_thread_wake_joiners(t);
    reg_remove(t);   /* collectable once no user reference remains */
    RUNNING_DEC();
    /* a main waiting for nothing to be left (the exit drain, or a join that
       can no longer finish) is woken by the last helper to go idle
       (sp_out_enter_locked), not here: that took a look at the counts after
       every thread that stopped */
    if (do_report) {
      if (locked) SCHED_UNLOCK();
      sp_thread_report(t);
      if (locked) SCHED_LOCK();
    }
    return;
  }
  /* t yielded back and is now fully off its stack. Only NOW is it safe for
     another worker to run it, so this is where it (re-)enters the run queue:
     - BLOCKED: it parked on a wait list. If a waker raced in while it was still
       switching out, it deferred the enqueue to us (wake_pending); do it now.
     - otherwise (still RUNNING): it yielded via Thread.pass and wants to keep
       running, so requeue it. */
  if (SP_TH_STATE(t) == SP_TH_BLOCKED) {
    /* off its stack now. PARKING -> PARKED, unless a wake came in while
       it was switching out (WOKEN): then it is ours to queue. */
    int e = SP_PK_PARKING;
    if (!SP_ATOMIC_CAS(&t->park, &e, SP_PK_PARKED, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      SP_ATOMIC_STORE(&t->park, SP_PK_RUN, __ATOMIC_SEQ_CST);
      SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
      runq_requeue(t);   /* pinned to us: our own loop re-picks it */
    }
  }
  else {
    /* Thread.pass / preempt: requeue. A started thread is pinned to its home
       worker (TLS affinity), so it lands on our own local queue TAIL -- other
       queued work still runs first, and the 61-tick global check, with the
       turn a preempted run takes (g_run_preempted), keeps the global queue
       from starving. */
    runq_requeue(t);
    sched_wake_home(t->home_wid);   /* pinned to us: our own loop re-picks it */
  }
  RUNNING_DEC();
  /* a main waiting for nothing to be left is woken by the last helper to go
     idle (sp_out_enter_locked) */
}

#ifdef SP_THREADS
/* Cooperative preemption point (called from sp_safepoint with the lock held). If
   the monitor flagged the running green thread as over its timeslice, yield to
   the fiber that ran it (sp_sched_home) exactly as Thread.pass does for a
   spawned thread: stay RUNNING so run_thread_once requeues us at the tail and
   the worker runs a sibling. The main thread is never flagged (the monitor
   only times green threads it runs via run_thread_once), so this only ever
   preempts a spawned thread. PRE/POST: lock held. */
static void sp_safepoint_preempt(void) {
#ifdef SP_CEXT
  /* Recursive ownership belongs to the worker, so do not run a different
     green thread while this worker is executing extension code. */
  if (sp_cext_lock_depth()) return;
#endif
  sp_thread *self = g_current;
  /* SPINEL_WORKERS=1 (#9840): main is the only worker, and the monitor
     asks it, past a slice of its own code with a thread runnable, to give
     each runnable thread one slice, as Thread.pass from main does */
  if (self == &g_main_thread && g_worker_cap == 0 && SP_ATOMIC_LOAD(&self->preempt_request, __ATOMIC_ACQUIRE)) {
    SP_ATOMIC_STORE(&self->preempt_request, 0, __ATOMIC_RELEASE);
    g_npreempt--;
    sp_recompute_safepoint_flag();
    sp_sched_pass();
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* a #raise one of them aimed at main */
    SCHED_LOCK();
    return;
  }
  if (!self || self == &g_main_thread || !SP_ATOMIC_LOAD(&self->preempt_request, __ATOMIC_ACQUIRE)) return;
  SP_ATOMIC_STORE(&self->preempt_request, 0, __ATOMIC_RELEASE);
  g_npreempt--;
  sp_recompute_safepoint_flag();
  /* Inside a fiber the thread resumed (an Enumerator's), switching out
     would park that fiber, and the thread is resumed at its own. Skip this
     slice; the monitor asks again on the next one. */
  if (sp_fiber_current != self->fiber) return;
  g_run_preempted = 1;
  SCHED_UNLOCK();
  sp_sched_switch_out();
  sp_fiber_fire_inject_if_pending();   /* a #kill/#raise delivered while we were off-cpu */
  SCHED_LOCK();
}
#endif

/* Run runnable green threads on this (the main) worker. Returns when `target`
   dies, when the main thread is woken back to RUNNABLE, or when the run queue is
   empty. When may_wait is set and a helper worker is still busy (so it may yet
   enqueue work or wake us), block on g_sched_work instead of returning on an
   empty queue -- otherwise main would falsely declare a deadlock while a helper
   runs the very thread that will wake it. At N=1 the running count is 0 between runs, so
   this returns on an empty queue exactly as before. PRE/POST: sched lock held. */
/* may_wait: 0 never waits, 1 waits for anything outstanding, 2 is the EXIT
   DRAIN -- it waits for work that can still run and NOT for threads that are
   merely blocked. main() finishing is the end of the program in CRuby, where
   the other threads are killed where they stand; waiting on a sleeper there
   meant `Thread.new { sleep 30 }` at the top of a script hung the process
   after its last statement, and `exit 0` was the difference between a program
   that ended and one that did not (#4394, #4397, rofl0r). */
static void sp_sched_pump(sp_thread *target, int may_wait) {
  for (;;) {
#ifdef SP_THREADS
    if (g_stw_active) { sp_stw_park_locked(); continue; }   /* park main through STW too */
#endif
    if (target && SP_TH_STATE(target) == SP_TH_DEAD) return;
    /* main joining a thread: a #raise aimed at main ends the join (main is on
       no wait list here, so nothing marks it RUNNABLE for it) */
    if (target && sp_thread_inject_pending(&g_main_thread)) return;
#ifdef SP_EV_BACKEND
    if (g_wslot[0].evfd > 0) sp_ev_worker_wait(0, 0);   /* see sp_worker_main */
#endif
    /* main blocked on a Queue/Mutex and a runnable thread just woke it */
    if (g_main_thread.state == SP_TH_RUNNABLE) { SP_TH_SET_STATE(&g_main_thread, SP_TH_RUNNING); return; }
    /* Run a green thread on the main worker only at N=1. With helpers present,
       main does NOT pump general green threads: running one means transferring
       off the root and back, which leaves the root fiber's published top-level
       roots a stale snapshot a collector could later mark. Main instead waits
       and lets a helper run the work. At N=1 there are no helpers, so it must
       pump. EXCEPTION: a thread pinned to main's worker (home_wid 0 -- it first
       ran here before main had a helper) can run NOWHERE else:
       helpers must not resume it (TLS affinity) and stealing skips it. If one
       is requeued while main pumps (a #kill/#raise or Queue wake unblocked it),
       main is the only worker that can finish it, so run it here -- otherwise
       the pump and that thread deadlock waiting on each other. */
#ifdef SP_THREADS
    if (sp_active_workers > 1) {
      RQ_LOCK(0); sp_thread *t = runq_pop(&g_lrq[0]); RQ_UNLOCK(0);
      if (t) { run_thread_once(t); continue; }
    }
    else
#endif
    {
      sp_thread *t = sched_pick(sp_worker_id);
      if (t) { run_thread_once(t); continue; }
    }
#ifdef SP_THREADS
    /* Queue empty for us. Wait while a helper is still running a green thread or
       work sits in the queue (a helper will pick it up) -- it may enqueue more or
       wake us; the last helper to go idle with nothing left wakes us
       (sp_sched_signal_if_quiescent). Only when nothing runs and the
       queue is empty do we fall through -- drained, or a deadlock the caller
       observes. */
    /* A thread on its way between the run counts and an I/O wait or a timed
       wait is in one of them at every moment -- its wake counts it runnable
       before it leaves the wait's count, its park enters that count before it
       stops running -- and the four are read as of one instant
       (counts_snapshot). Counts that kept changing while it read them say
       there is work. */
    if (may_wait) SP_ATOMIC_STORE(&g_main_waits, 1, __ATOMIC_SEQ_CST);   /* before the counts: see there */
    long long sum[4];
    int read = counts_snapshot(sum);
    int outstanding = !read || sum[0] != 0 || sum[1] != 0;
    /* A timed wait or an I/O waiter is work that may yet become runnable, so an
       ordinary wait counts it. The exit drain does not: nothing is going to
       ask for it after main has returned. */
    if (may_wait == 1) outstanding = outstanding || g_io_waiters || sum[2] != 0 || sum[3] != 0;
    if (may_wait && outstanding) {
#ifdef SP_EV_BACKEND
      /* Main is worker 0, and threads pin to it -- at SPINEL_WORKERS=1 all of
         them do. So main waits on worker 0's readiness set like any other
         worker, or nothing would ever deliver to the threads pinned here
         (#4306). The timeout keeps the pump's own predicates -- target death,
         main made runnable -- checked on a bound. */
      if (g_wslot[0].evfd > 0) { sp_ev_worker_wait(0, SP_EV_BACKSTOP_MS); continue; }
#endif
      sp_out_enter_locked(0, SP_OUT_IDLE);
      pthread_cond_wait(&g_sched_work, &g_sched_lock);
      sp_out_leave_locked(0);
      continue;
    }
#else
    (void)may_wait;
#endif
    return;
  }
}

#ifdef SP_THREADS
/* The first thread on worker 0's queue that is pinned there, off the queue. An
   unstarted one stays for a helper to steal: after a helper that failed to
   start, a thread running here can have spawned one onto this queue. PRE: the
   queue's lock held. */
static sp_thread *runq_pop_pinned(sp_runq *q) {
  if (!q->head || q->head->home_wid >= 0) return runq_pop(q);
  sp_thread *prev = q->head;
  while (prev->rq_next && prev->rq_next->home_wid < 0) prev = prev->rq_next;
  sp_thread *t = prev->rq_next;
  if (!t) return NULL;
  prev->rq_next = t->rq_next;
  if (q->tail == t) q->tail = prev;
  if (q->woke == t) q->woke = NULL;
  t->rq_next = NULL;
  RUNNABLE_DEC();
  return t;
}

/* What main's sweep runs when nothing is pinned to its worker. A thread a helper
   spawned waits on that helper's queue for a worker to come round to it, and a
   helper in a blocking call does not. A new helper takes it, when the pool can
   grow. When it cannot and every helper is in such a call, main does, the one
   worker left, and the thread is pinned here. */
static sp_thread *sched_pass_stranded(void) {
  if (!SP_ATOMIC_LOAD(&g_lrq_unstarted, __ATOMIC_SEQ_CST)) return NULL;
  int before = g_helpers_spawned;
  sp_sched_maybe_grow();
  if (g_helpers_spawned > before) return NULL;
  for (int i = 1; i < sp_active_workers; i++)
    if (SP_ATOMIC_LOAD(&g_native_out[i], __ATOMIC_RELAXED) != SP_OUT_NATIVE) return NULL;
  return sched_pick(sp_worker_id);
}
#endif

/* One round-robin sweep for Thread.pass from the main thread: give each thread
   that is runnable *right now* exactly one timeslice, then return to main.
   Threads that re-queue themselves (their own Thread.pass) during the sweep land
   after the snapshot boundary and wait for the next sweep, so a sibling looping
   on Thread.pass cannot starve main (which would happen if main drained the
   queue here). */
static void sp_sched_pass(void) {
  /* Snapshot the runnable count and run exactly that many: a thread that
     re-queues itself (its own Thread.pass) during the sweep lands beyond the
     snapshot and waits for the next sweep, so it cannot starve main. */
  int n, nrun;
  counts_approx(&n, &nrun);
  (void)nrun;
#ifdef SP_EV_BACKEND
  /* Main's own readiness set holds the I/O of the threads pinned to main's
     worker (one that first ran in a sweep with no helper is), and only a
     drain of it readies them. The pump drains it; a Thread.pass loop did not,
     so main spinning on Thread.pass until such a thread got a connection
     spun for good. A drain is a few syscalls (about 12 us on Darwin), and a
     pass loop is often a busy wait at a few ns a sweep, so it is taken on
     every 1024th sweep and only while some thread waits on I/O: the event
     still arrives within microseconds of spinning. */
  static unsigned pass_tick;   /* main's only: under the lock */
  if (g_wslot[0].evfd > 0 && (++pass_tick & 1023u) == 0 && nio_approx()) {
    sp_ev_worker_wait(0, 0);
    counts_approx(&n, &nrun);
  }
#endif
  while (n-- > 0) {
    sp_thread *t;
#ifdef SP_THREADS
    /* With helpers main starts no thread, as in its pump: a thread that first
       runs here is pinned to main's worker, and stands still whenever main is
       in a blocking C call, for as long as that call lasts. A thread spawned
       just before the Thread.pass found the new helper not up yet, so it took
       that road every time. */
    if (sp_active_workers > 1) {
      RQ_LOCK(0); t = runq_pop_pinned(&g_lrq[0]); RQ_UNLOCK(0);
      if (!t) t = sched_pass_stranded();
    }
    else
#endif
    t = sched_pick(sp_worker_id);
    if (!t) return;
    run_thread_once(t);
  }
}

static void sp_thread_scan(void *p) {
  sp_thread *t = (sp_thread *)p;
  if (t->fiber) sp_gc_mark(t->fiber);
  if (t->at) sp_gc_mark(t->at);
  sp_mark_rbval(t->arg);
  sp_mark_rbval(t->retval);
  sp_mark_rbval(t->name);
  if (t->exc_obj) sp_gc_mark(t->exc_obj);
  if (t->tls) sp_gc_mark(t->tls);
}

/* lib/sp_cold.c: a delivered Signal.trap before the first thread runs in
   the waits below, as the single-threaded runtime's does (#10210) */
extern volatile int sp_sig_pending_flag;
void sp_sig_run_pending(void);
void sp_sig_threads_began(void);
static inline void sp_sig_run_if_pending(void) {
  if (SP_ATOMIC_LOAD(&sp_sig_pending_flag, __ATOMIC_ACQUIRE)) sp_sig_run_pending();
}
sp_thread *sp_Thread_spawn_fiber_at(sp_Fiber *f, sp_RbVal arg, const char *file, sp_int line) {SP_GC_ROOT_RBVAL(arg);SP_GC_ROOT(f);SP_GC_ROOT_STR(file);
  sp_thread *t = sp_Thread_spawn_fiber(f, arg);
  t->birth_file = file;
  t->birth_line = line;
  return t;
}
sp_thread *sp_Thread_spawn_fiber(sp_Fiber *f, sp_RbVal arg) {
#ifdef SP_THREADS
  if (!g_workers_started) {
    if (!sp_sched_ensure_workers())
      sp_raise_cls("ThreadError", "failed to start scheduler monitor");
    sp_sig_threads_began();   /* a trap set before now moves to its own thread (lib/sp_cold.c) */
  }
#endif
  SP_GC_ROOT(f);   /* root the freshly-built fiber across the allocation below */
  SP_GC_ROOT_RBVAL(arg);
  f->owner = 0;   /* the thread's own fiber: the scheduler runs it on any worker */
  f->thread_main = 1;
  sp_thread *volatile t = (sp_thread *)sp_gc_alloc(sizeof(sp_thread), NULL, sp_thread_scan);
  memset(t, 0, sizeof *t);
  t->home_wid = -1;              /* not yet started: any worker may pick it up */
  t->fiber = f;
  t->arg = arg;
  t->retval = sp_box_nil();
  t->name = sp_box_nil();
  /* CRuby: a new thread starts at its creator's priority */
  t->priority = g_current ? g_current->priority : 0;
  t->report_on_exception = g_report_default;
  SCHED_LOCK();
  t->id = g_next_id++;
  reg_add(t);
  /* Prefer the spawning worker's local queue (locality) -- but only when that
     worker actually drains it. Main is a coordinator: it hands work to helpers
     and only runs a thread itself in the sole-worker fallback (no helper could
     be spawned), which drains the global queue too. So a main-spawned thread
     always goes to the global queue -- never main's local queue, where it would
     wait on a steal. (g_worker_cap > 0 whenever threads are in use.) */
#ifdef SP_THREADS
  /* (#8702) main places the new thread on its worker now, as
     the picker would (sched_place_unstarted): onto that worker's own queue,
     which it runs without the scheduler lock. */
  if (g_current == &g_main_thread && g_worker_cap > 0) {
    if (!sched_place_unstarted(0, t)) { rq_push(t); SCHED_WAKE(); }
  }
  else
#endif
  { lrq_push(sp_worker_id, t); SCHED_WAKE(); }   /* a helper worker may be idle: hand it the new thread */
  SCHED_UNLOCK();
  return t;
}

/* Joining the current thread can never finish. Reject it before attempting to
   park, including on the main thread before the timer monitor has started. */
static void sp_thread_check_join_target(sp_thread *t) {
  if (t == g_current)
    sp_raise_cls("ThreadError", "Target thread must not be current thread");
}

/* Block the calling thread until `t` is dead, on t's joiners, under t's own
   wait lock: its death takes the list under that lock after storing DEAD, so
   a joiner that finds t alive under it is woken (sp_thread_wake_joiners). A
   spawned thread parks there and yields to the scheduler. Main enlists the
   same way and waits in its pump until the death makes it runnable; it does
   not wait for t to read DEAD, which comes before the wake, or the late wake
   would mark a running main runnable. */
static void sp_thread_await(sp_thread *t) {
  sp_thread_check_join_target(t);
  if (SP_TH_STATE(t) == SP_TH_DEAD) return;
  sp_thread *self = g_current;
  wl_lock(&t->join_wl);
  if (SP_TH_STATE(t) == SP_TH_DEAD) { wl_unlock(&t->join_wl); return; }
  if (self != &g_main_thread) {
    sp_sched_block_wl(&t->joiners, &t->join_wl, 0, NULL);   /* resumes once t is dead */
    wl_unlock(&t->join_wl);
    return;
  }
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  self->wait_head = &t->joiners;
  SP_ATOMIC_STORE(&self->wait_lock, &t->join_wl, __ATOMIC_RELEASE);
  self->wait_next = NULL;
  { sp_thread **pp = &t->joiners; while (*pp) pp = &(*pp)->wait_next; *pp = self; }
  wl_unlock(&t->join_wl);
  /* A #raise aimed at main is queued under the scheduler lock, and wakes a
     BLOCKED main off this list (sp_thread_deliver): one queued before main
     parked is seen here instead, under the same lock. */
  SCHED_LOCK();
  int raised = sp_thread_inject_pending(self);
  if (!raised) sp_sched_pump(NULL, 1);   /* returns once t's death, or a #raise, marks main RUNNABLE */
  int woken = (SP_TH_STATE(self) == SP_TH_RUNNING);
  SCHED_UNLOCK();
  if (!woken) {
    /* Off the list. If a death has taken main off it already, its wake is on
       the way: wait for that, or it would mark a running main RUNNABLE. */
    int taken;
    wl_lock(&t->join_wl);
    taken = self->wait_head != &t->joiners;
    if (!taken) {
      sp_thread **pp = &t->joiners;
      while (*pp && *pp != self) pp = &(*pp)->wait_next;
      if (*pp) *pp = self->wait_next;
      self->wait_next = NULL; self->wait_head = NULL; SP_ATOMIC_STORE(&self->wait_lock, NULL, __ATOMIC_RELEASE);
    }
    wl_unlock(&t->join_wl);
    if (taken) { SCHED_LOCK(); sp_sched_pump(NULL, 1); SCHED_UNLOCK(); }
    SP_TH_SET_STATE(self, SP_TH_RUNNING);
  }
  sp_fiber_fire_inject_if_pending();   /* a #raise that ended the join */
  /* nothing can run and t is alive: a deadlock. A death in progress still
     counts as running, so the pump does not return for nothing while one is. */
  if (!woken && !raised && SP_TH_STATE(t) != SP_TH_DEAD)
    sp_raise_cls("ThreadError", "deadlock detected: no runnable thread");
}

/* CRuby: #join and #value re-raise the thread's unhandled exception in the
   joining thread. */
static void sp_thread_reraise_if_exc(sp_thread *t) {
  if (!t->has_exc) return;
  /* An exit this thread forwarded to main and main has not met yet is this
     same exception: raised from the slot it goes once, and a later wait does
     not raise it again. A later join finds the slot empty and re-raises. */
  sp_fiber_fire_inject_if_pending();
  sp_fiber_reraise(t->exc_cls, t->exc_msg, t->exc_obj);
}

sp_thread *sp_Thread_join(sp_thread *t) {
  sp_thread_await(t);
  sp_thread_reraise_if_exc(t);
  return t;
}

/* CRuby's Thread#join(limit): wait at most `seconds` for the thread to
   finish, answering the thread when it does and NULL (nil) on timeout.
   Same return type as the no-arg join so one variable can hold either
   call's result. */
static int sp_thread_is_dead(void *t) { return SP_TH_STATE((sp_thread *)t) == SP_TH_DEAD; }
sp_thread *sp_Thread_join_timeout(sp_thread *t, double seconds) {
  sp_thread_check_join_target(t);
  if (SP_TH_STATE(t) == SP_TH_DEAD) {
    sp_thread_reraise_if_exc(t);
    return t;
  }
  if (!(seconds > 0)) return NULL;   /* also catches a NaN limit */

#ifdef SP_THREADS
  int woken = sp_sched_block_timeout(&t->joiners, sp_monotonic_now() + seconds, NULL, &t->join_wl, sp_thread_is_dead, t);
  if (woken < 0) sp_raise_cls("NoMemoryError", "failed to schedule thread join timeout");
  if (!woken) return NULL;
  sp_thread_reraise_if_exc(t);
  return t;
#else
  /* Without the monitor thread there is no timer queue to park on. Keep the
     single-worker build responsive by yielding in short sleeps until either
     the target finishes or the deadline passes. */
  const double poll_slice = 0.002;
  double deadline = sp_monotonic_now() + seconds;
  for (;;) {
    double left = deadline - sp_monotonic_now();
    if (left <= 0) break;
    sp_sleep(left < poll_slice ? (sp_float)left : (sp_float)poll_slice);
    SCHED_LOCK();
    int dead = (SP_TH_STATE(t) == SP_TH_DEAD);
    SCHED_UNLOCK();
    if (dead) { sp_thread_reraise_if_exc(t); return t; }
  }
  return NULL;
#endif
}

sp_RbVal sp_Thread_value(sp_thread *t) {
  sp_thread_await(t);
  sp_thread_reraise_if_exc(t);
  return t->retval;
}

/* lib/sp_cold.c: the single-threaded runtime runs a delivered Signal.trap at
   the waits that hand the scheduler round, a read's park among them; the
   threaded one raises the flag only before its first thread (its trap thread
   runs them afterwards) */
void sp_Thread_pass(void) {
  if (SP_ATOMIC_LOAD(&sp_sig_pending_flag, __ATOMIC_ACQUIRE)) sp_sig_run_pending();
  SCHED_LOCK();
  sp_thread *self = g_current;
  if (self == &g_main_thread) {
    sp_sched_pass();   /* one round-robin sweep, then main resumes (not a drain) */
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* a #raise one of them aimed at main */
  }
  else {
    /* Yield but stay runnable. Do NOT enqueue ourselves here: a second worker
       could pop and run our fiber while we are still mid-context-switch. We keep
       our state RUNNING and transfer to the fiber that ran us; run_thread_once
       requeues us once we are fully off-cpu. */
    SCHED_UNLOCK();
    sp_sched_switch_out();
    sp_fiber_fire_inject_if_pending();   /* a #kill/#raise delivered while paused */
  }
}

sp_thread *sp_Thread_current(void) { return g_current; }
/* The fiber a finished transferred fiber hands back to: the green thread's
   own fiber, or NULL on the main thread (its root fiber). */
sp_Fiber *sp_thread_main_fiber(void) { return g_current ? g_current->fiber : NULL; }
/* The current thread as a fiber owner. An id is never reused, so a fiber
   can't be taken over by a later thread at the same address. */
unsigned sp_thread_owner_id(void) { return g_current ? g_current->id + 1 : 0; }

sp_bool sp_Thread_alive(sp_thread *t) { return SP_TH_STATE(t) != SP_TH_DEAD; }

/* Thread.report_on_exception=(v): set the default for threads spawned after.
   Thread.report_on_exception: read the default. Per-thread #report_on_exception
   reads/sets the thread's own flag. */
sp_bool sp_Thread_set_report_default(sp_bool v) { g_report_default = v ? 1 : 0; return v; }
sp_bool sp_Thread_get_report_default(void) { return g_report_default; }
sp_bool sp_Thread_set_report(sp_thread *t, sp_bool v) { t->report_on_exception = v ? 1 : 0; return v; }
sp_bool sp_Thread_get_report(sp_thread *t) { return t->report_on_exception; }

sp_thread *sp_Thread_main(void) { return &g_main_thread; }

sp_RbVal sp_Thread_get_name(sp_thread *t) { return t->name; }
sp_RbVal sp_Thread_set_name(sp_thread *t, sp_RbVal v) { if (v.tag == SP_TAG_STR) sp_str_retained(v.v.s); sp_gc_wb((void*)t); t->name = v; return v; }
/* #priority= as CRuby's rb_thread_priority_set: the Integer value (a Float
   truncates, nil is a TypeError: the generated call converts), clamped to
   -3..3. CRuby treats it as a
   hint to its scheduler and so does this one: it is kept and answered, and
   the green-thread scheduler does not reorder by it. The call answers its
   argument, as an assignment does. */
sp_int sp_Thread_get_priority(sp_thread *t) { return t->priority; }
void sp_Thread_set_priority(sp_thread *t, sp_int p) {
  t->priority = p > 3 ? 3 : (p < -3 ? -3 : (int)p);
}

/* Thread.list enumeration: the main thread followed by every live spawned
   thread. A dead one leaves the registry a moment after it stores DEAD and
   wakes its joiners, which no longer wait for the scheduler lock (#9216):
   a joiner that runs at once must not count it, so DEAD is skipped here
   too. The generated TU builds the array over these accessors since it
   owns sp_PolyArray. */
static int thread_listed(sp_thread *t) { return !t->internal && SP_TH_STATE(t) != SP_TH_DEAD; }
sp_int sp_Thread_list_count(void) {
  sp_int n = 1;   /* the main thread */
  REG_LOCK();
  for (sp_thread *t = g_all; t; t = t->all_next) if (thread_listed(t)) n++;
  REG_UNLOCK();
  return n;
}
sp_thread *sp_Thread_list_at(sp_int i) {
  if (i <= 0) return &g_main_thread;
  REG_LOCK();
  sp_thread *t = g_all;
  while (t && !thread_listed(t)) t = t->all_next;
  for (sp_int k = 1; t && k < i; k++) do t = t->all_next; while (t && !thread_listed(t));
  REG_UNLOCK();
  return t ? t : &g_main_thread;
}
void sp_thread_set_internal(sp_thread *t) { t->internal = 1; }

/* #status: "run" while runnable/running, "sleep" while blocked, false when it
   finished normally, nil when it died with an unhandled exception. */
sp_RbVal sp_Thread_status(sp_thread *t) {
  switch (SP_TH_STATE(t)) {
    case SP_TH_RUNNING: case SP_TH_RUNNABLE: return sp_box_str(&("\xff" "run")[1]);
    case SP_TH_BLOCKED:                       return sp_box_str(&("\xff" "sleep")[1]);
    default:                                  return t->has_exc ? sp_box_nil() : sp_box_bool(0);
  }
}

/* ---- thread-local storage (Thread#[] / #[]=), a small sym->value map ---- */
typedef struct { sp_sym *keys; sp_RbVal *vals; sp_int len, cap; } sp_tls_map;
static void sp_tls_scan(void *p) { sp_tls_map *m = (sp_tls_map *)p; for (sp_int i = 0; i < m->len; i++) sp_mark_rbval(m->vals[i]); }
static void sp_tls_fin(void *p)  { sp_tls_map *m = (sp_tls_map *)p; free(m->keys); free(m->vals); }

sp_RbVal sp_Thread_tls_get(sp_thread *t, sp_sym k) {
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (sp_int i = 0; i < m->len; i++) if (m->keys[i] == k) return m->vals[i];
  return sp_box_nil();
}
sp_bool sp_Thread_tls_key(sp_thread *t, sp_sym k) {
  /* A READ needs no barrier, and this one was pointed at the thread anyway --
     which for the main thread is a static struct, so the barrier read the byte
     in front of a global to decide whether it had a header. */
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (sp_int i = 0; i < m->len; i++) if (m->keys[i] == k) return 1;
  return 0;
}
/* The barrier belongs on the MAP, which is the object that ends up holding the
   new reference. It was on the thread: a minor mark then reached the map
   through the thread's scan and MARKED it, but an old object is not re-scanned
   unless it is in the remembered set, so a young value stored into a long-lived
   map was swept out from under it. `Thread.current[:slots] = {}` per request,
   on a main thread whose map has been alive since boot, is exactly that shape:
   the next read of the slot faulted in sp_PolyPolyHash_get (#4311). */
sp_RbVal sp_Thread_tls_set(sp_thread *t, sp_sym k, sp_RbVal v) {
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (sp_int i = 0; i < m->len; i++) if (m->keys[i] == k) {
    if (v.tag == SP_TAG_NIL) {
      memmove(&m->keys[i], &m->keys[i + 1], sizeof(sp_sym) * (size_t)(m->len - i - 1));
      memmove(&m->vals[i], &m->vals[i + 1], sizeof(sp_RbVal) * (size_t)(m->len - i - 1));
      m->len--;
      return v;
    }
    m->vals[i] = v; sp_gc_wb((void *)m); return v;
  }
  if (v.tag == SP_TAG_NIL) return v;
  if (!m) {
    SP_GC_ROOT(t); SP_GC_ROOT_RBVAL(v);
    m = (sp_tls_map *)sp_gc_alloc(sizeof(sp_tls_map), sp_tls_fin, sp_tls_scan);
    m->cap = 4; m->len = 0;
    m->keys = (sp_sym *)malloc(sizeof(sp_sym) * m->cap);
    m->vals = (sp_RbVal *)malloc(sizeof(sp_RbVal) * m->cap);
    if (!m->keys || !m->vals) sp_raise_cls("NoMemoryError", "failed to allocate thread storage");
    t->tls = m;
    /* The map is a second reference store, into the THREAD, and the barrier
       below covers only the map. sp_gc_alloc above is what makes this needed:
       it can collect, which clears t's dirty bit, so a thread that was already
       old holds a young map that nothing recorded and a minor mark never walks
       it. The whole map went away and the next read answered nil. */
    sp_gc_wb((void *)t);
  }
  if (m->len == m->cap) {
    sp_int nc = m->cap * 2;
    sp_sym *nk = (sp_sym *)realloc(m->keys, sizeof(sp_sym) * nc);
    if (!nk) sp_raise_cls("NoMemoryError", "failed to grow thread storage");
    m->keys = nk;
    sp_RbVal *nv = (sp_RbVal *)realloc(m->vals, sizeof(sp_RbVal) * nc);
    if (!nv) sp_raise_cls("NoMemoryError", "failed to grow thread storage");
    m->vals = nv; m->cap = nc;
  }
  m->keys[m->len] = k; m->vals[m->len] = v; m->len++;
  sp_gc_wb((void *)m);
  return v;
}
sp_PolyArray *sp_Thread_tls_keys(sp_thread *t) {
  SP_GC_ROOT(t);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (sp_int i = 0; i < m->len; i++) sp_PolyArray_push(a, sp_box_sym(m->keys[i]));
  return a;
}

#ifdef SP_THREADS
/* ---- helper OS workers (design 3.2, Appendix B) ---- */
static pthread_t g_worker_threads[SP_MAX_WORKERS];

#ifdef __linux__
/* The CPU quota a cgroup gives the process, rounded up, or 0 for none:
   cgroup v2's cpu.max ("200000 100000" is two CPUs, "max ..." is none),
   else v1's cfs_quota_us / cfs_period_us (a quota of -1 is none). */
static int sp_cgroup_cpu_quota(void) {
  long quota = -1, period = 0;
  FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (f) {
    char q[32];
    if (fscanf(f, "%31s %ld", q, &period) == 2 && strcmp(q, "max") != 0) quota = atol(q);
    fclose(f);
  }
  else {
    f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r");
    if (f) { if (fscanf(f, "%ld", &quota) != 1) quota = -1; fclose(f); }
    f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
    if (f) { if (fscanf(f, "%ld", &period) != 1) period = 0; fclose(f); }
  }
  if (quota <= 0 || period <= 0) return 0;
  long n = (quota + period - 1) / period;
  return n > 0 ? (int)n : 1;
}
#endif

/* The CPUs this process may run on: the online ones, narrowed by its
   affinity mask and a cgroup's CPU quota (a container's --cpus), as
   CRuby's Etc.nprocessors and Go's GOMAXPROCS narrow them. Sized from the
   host's count alone, a 2-CPU container on an 18-CPU host ran 18 workers
   (#8206). */
static int sp_available_cpus(void) {
  long c = sysconf(_SC_NPROCESSORS_ONLN);
  int n = (c > 0) ? (int)c : 1;
#ifdef __linux__
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof set, &set) == 0) {
    int a = CPU_COUNT(&set);
    if (a > 0 && a < n) n = a;
  }
  int q = sp_cgroup_cpu_quota();
  if (q > 0 && q < n) n = q;
#endif
  return n;
}

/* min(available CPUs, SPINEL_WORKERS); the env var overrides the autodetect. */
static int sp_worker_count(void) {
  const char *e = getenv("SPINEL_WORKERS");
  int n;
  if (e && *e) { n = atoi(e); if (n < 1) n = 1; }
  else n = sp_available_cpus();
  if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  return n;
}

/* The monitor thread (sysmon, design §5). Two jobs, both off the workers' backs:
   (1) fire scheduler timers (sleep, timed I/O, and timed condition waits), and (2) enforce the
   timeslice -- when a worker has run the same green thread past the quantum, flag
   it for preemption and SIGURG the worker so it yields at its next safepoint. It
   idles on g_sysmon_cv when nothing sleeps and no worker runs a green thread, and
   otherwise ticks on a short nanosleep. It never participates in GC. */
/* A thread a worker started after this turn read the slots, seen once
   g_sysmon_wake_at is out: go round instead of waiting, and time it (see
   g_sysmon_wake_at). Only for a turn that found every worker idle -- a busy
   one waits a tick. PRE: the monitor. */
static int sp_sysmon_missed_start(int busy) {
  if (busy) return 0;
  for (int i = 0; i < sp_active_workers; i++)
    if (g_wslot[i].active && SP_ATOMIC_LOAD(&g_wslot[i].cur, __ATOMIC_SEQ_CST)) return 1;
  return 0;
}
static void *sp_sysmon_main(void *arg) {
  (void)arg;
  /* The monitor must never field a preemption signal itself. */
  sigset_t blk; sigemptyset(&blk); sigaddset(&blk, g_preempt_sig);
  pthread_sigmask(SIG_BLOCK, &blk, NULL);
  g_cnt_me = &g_cnt[SP_CNT_MON];   /* the threads its timers make runnable */
  double last_busy = 0.0;   /* when a worker was last seen running a thread */
  double main_since = 0.0;  /* SPINEL_WORKERS=1: since when main's own code has run beside a runnable thread */
  SCHED_LOCK();
  for (;;) {
    if (g_shutdown) break;
    /* Stay out of the scheduler state while a collection has the world stopped:
       the monitor is not a GC participant (it holds no roots) but it must not
       move threads between lists concurrently with the collector. */
    if (g_stw_active) { pthread_cond_wait(&g_stw_release, &g_sched_lock); continue; }
    SP_STAT_ADD(g_mon_iters, 1);
    double now = sp_monotonic_now();
    if (sched_lat_enabled()) {
      static double last_report = 0;
      if (now - last_report >= 5.0) {
        last_report = now;
        fprintf(stderr, "[sched-lat] n=%llu max=%.0fus lrq_max=%d  us:", g_sched_lat_n, g_sched_lat_max, g_lrq_len_max);
        for (int k = 0; k < 32; k++) if (g_sched_lat_hist[k]) fprintf(stderr, " [%u]=%llu", 1u << k, g_sched_lat_hist[k]);
        fprintf(stderr, "\n");
        int busyw = 0; for (int i = 0; i < sp_active_workers; i++) if (g_wslot[i].active && SP_ATOMIC_LOAD(&g_wslot[i].cur, __ATOMIC_ACQUIRE)) busyw++;
        { int crq, cru; counts_approx(&crq, &cru);
          fprintf(stderr, "[sched-lat] workers=%d busy=%d runnable=%d nrunning=%d\n", sp_active_workers, busyw, crq, cru); }
        { int pinned[SP_MAX_WORKERS] = {0}; int unpinned = 0;
          REG_LOCK();
          for (sp_thread *a = g_all; a; a = a->all_next) { if (a->home_wid >= 0) pinned[a->home_wid]++; else unpinned++; }
          REG_UNLOCK();
          fprintf(stderr, "[sched-lat] pinned per worker:"); for (int i = 0; i < sp_active_workers; i++) fprintf(stderr, " %d", pinned[i]); fprintf(stderr, "  unpinned=%d\n", unpinned); }
        sched_hist_print("mutex-wait", g_mtx_hist, g_mtx_n, g_mtx_max);
        fprintf(stderr, "[mutex-wait] fast-path locks=%llu spin-acquired=%llu\n", g_mtx_fast, g_mtx_spin);
        sched_hist_print("cv-wait", g_cv_hist, g_cv_n, g_cv_max);
        g_lrq_len_max = 0;
      }
    }
    sp_timer_fire_due(now);
    double nearest = sp_timer_next();
    /* the clock again: the wait below is `nearest - now`, and waking what was
       due takes time, which a stale `now` added to the wait -- every timer
       of the next batch fired that much late */
    now = sp_monotonic_now();
    int npf = 1, nio = 0, busy = 0;
    /* Timeslice enforcement, on every turn: it reads the worker slots, not the
       wait lists, so it is O(workers) and does not belong behind the skip
       below. (An earlier cut jumped over its declarations, which is how `busy`
       came to be read uninitialised.) */
    for (int i = 0; i < sp_active_workers; i++) {
      sp_thread *r = g_wslot[i].active ? SP_ATOMIC_LOAD(&g_wslot[i].cur, __ATOMIC_ACQUIRE) : NULL;
      if (!r) continue;
#ifdef SP_CEXT
      /* Repeat I/O nudges until unregister closes the lifetime of UBF data.
         This covers cancellation arriving just before the native syscall. */
      if(r->cext_repeat_unblock && r->cext_unblock && r->fiber && sp_fiber_inject_pending(r->fiber))
        r->cext_unblock(r->cext_unblock_data);
#endif
      busy = 1; last_busy = now;
      if (!SP_ATOMIC_LOAD(&r->preempt_request, __ATOMIC_ACQUIRE) && (now - wslot_since(i)) >= SP_PREEMPT_QUANTUM)
        sched_preempt(i, r);
    }
    /* SPINEL_WORKERS=1 (#9840): no helper runs a green thread, so main's own
       code is timed while a thread is runnable, and past the quantum main is
       asked to give the runnable threads their slices at its next safepoint
       (sp_safepoint_preempt). Main pumping (in a join, a sleep) runs them
       already, and a thread it runs is timed above. (Asked of the cap
       alone, set before this thread was created: g_workers_started goes up
       only once the start has returned, and a monitor that ran first read
       it as 0.) */
    if (g_worker_cap == 0) {
      int rq0, ru0; counts_approx(&rq0, &ru0);
      if (rq0 > 0 && !SP_ATOMIC_LOAD(&g_wslot[0].cur, __ATOMIC_ACQUIRE) &&
          SP_TH_STATE(&g_main_thread) == SP_TH_RUNNING) {
        busy = 1; last_busy = now;
        if (main_since == 0.0) main_since = now;
        else if (now - main_since >= SP_PREEMPT_QUANTUM &&
                 !SP_ATOMIC_LOAD(&g_main_thread.preempt_request, __ATOMIC_ACQUIRE)) {
          SP_ATOMIC_STORE(&g_main_thread.preempt_request, 1, __ATOMIC_SEQ_CST);
          g_npreempt++;
          sp_recompute_safepoint_flag();
#ifndef __APPLE__
          pthread_kill(g_wslot[0].tid, g_preempt_sig);   /* see sched_preempt */
#endif
          main_since = 0.0;
        }
      }
      else main_since = 0.0;
    }
    /* Rebuilding the poll set walks the I/O waiters and grows with the
       population. The event backend holds descriptors in the kernel, so skip
       that walk until a deadline is due; the timer heap still fires due
       handles above without scanning parked threads. */
    int skip_walks = 0;
#ifdef SP_EV_BACKEND
    { double gn = nearest_get();
      skip_walks = (g_ev_fd >= 0 && !(gn != 0.0 && now >= gn)); }
#endif
    if (skip_walks) {
      double gn = nearest_get();
      if (gn != 0.0 && (nearest == 0.0 || gn < nearest)) nearest = gn;
    }
    else {
    /* Build the I/O poll set: slot 0 is the wake pipe (a registering thread
       writes a byte to break us out of poll early), the rest are parked fds.
       Timed I/O waiters use the same timer heap as sleeps and timed condition
       waits; this list is only for readiness registration. */
#ifdef SP_EV_BACKEND
    /* the kernel holds the interest set: nothing to build, and have_io below
       reads the list head, so the waiters need no walk to be counted */
    if (g_ev_fd < 0)
#endif
    for (sp_thread **wp = &g_io_waiters; *wp; ) {
      sp_thread *w = *wp;
      wp = &w->wait_next;
      nio++;
      /* one slot per descriptor: the thread's own, or each of its set */
      int nslot = w->io_fd >= 0 ? 1 : w->io_nset;
      for (int si = 0; si < nslot; si++) {
        if (npf >= g_pcap) {
          int nc = g_pcap ? g_pcap * 2 : 16;
          struct pollfd *np = (struct pollfd *)realloc(g_pfds, sizeof(struct pollfd) * nc);
          sp_thread **nh = (sp_thread **)realloc(g_pths, sizeof(sp_thread *) * nc);
          if (np) g_pfds = np; if (nh) g_pths = nh;
          if (!np || !nh) break;
          g_pcap = nc;
        }
        if (w->io_fd >= 0) { g_pfds[npf].fd = w->io_fd; g_pfds[npf].events = w->io_events; }
        else { g_pfds[npf].fd = w->io_set[si].fd; g_pfds[npf].events = w->io_set[si].events; }
        g_pfds[npf].revents = 0;
        g_pths[npf] = w; npf++;
      }
    }
    if (g_pcap < 1) {   /* ensure room for slot 0 even with no I/O waiters */
      g_pfds = (struct pollfd *)realloc(g_pfds, sizeof(struct pollfd) * 16);
      g_pths = (sp_thread **)realloc(g_pths, sizeof(sp_thread *) * 16);
      if (g_pfds && g_pths) g_pcap = 16;
    }
    }
    int have_io = (nio > 0) || (g_io_waiters != NULL) || nio_approx();
    /* (#8702) Nothing to time or watch, and no worker seen busy for a few
       ticks. Going untimed as soon as nothing ran made every thread start
       signal the monitor under the scheduler lock: a short thread spawned
       and joined in a loop paid that on most rounds once the workers stopped
       holding the scheduler lock while idle, as the monitor then always got
       it between rounds. Ticking a little longer costs a few wakes when a
       program goes quiet. */
    if (nearest == 0.0 && !busy && !have_io && now - last_busy >= SP_MON_LINGER) {
      /* nothing to time or watch: sleep until a thread sleeps / waits on I/O /
         is picked up by a worker (a registrant signals g_sysmon_cv). */
      SP_ATOMIC_STORE(&g_sysmon_idle, 1, __ATOMIC_SEQ_CST);
      /* A worker starts a thread without the scheduler lock: it publishes the
         thread in its slot and then reads this flag (run_thread_once), and
         signals only if it reads 1. Here the flag goes up first and the slots
         are read again after it, so of the two one sees the other: the worker
         sees 1 and signals under the lock, which it can take only once this
         wait has released it, or this sees the thread and does not wait. */
      int started = 0;
      for (int i = 0; i < sp_active_workers; i++)
        if (g_wslot[i].active && SP_ATOMIC_LOAD(&g_wslot[i].cur, __ATOMIC_SEQ_CST)) { started = 1; break; }
      /* the same for a deadline added since the heap was read: a parker
         stores g_nearest and then reads the flag (sp_sysmon_nudge) */
      if (nearest_get() != 0.0) started = 1;
      /* (at SPINEL_WORKERS=1 a thread main makes runnable wakes no one: the
         monitor looks again a quantum later, to time main beside it. Its
         first round comes here at once, before the start that created it
         may have set g_workers_started: an untimed wait then left main
         spinning on a flag no thread ran to set) */
      if (!started && g_worker_cap == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double dt = SP_PREEMPT_QUANTUM;
        ts.tv_sec += (time_t)dt;
        ts.tv_nsec += (long)((dt - (double)(time_t)dt) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_sysmon_cv, &g_sched_lock, &ts);
      }
      else if (!started) pthread_cond_wait(&g_sysmon_cv, &g_sched_lock);
      SP_ATOMIC_STORE(&g_sysmon_idle, 0, __ATOMIC_SEQ_CST);
      last_busy = sp_monotonic_now();   /* woken for work: tick for a while */
    }
    else {
      /* Poll the parked fds, timing out at the quantum (while preempting), near
         the nearest sleeper deadline, or 50ms otherwise (poll returns earlier on
         fd activity or a wake-pipe byte). */
      double dt = busy ? SP_PREEMPT_TICK : (nearest != 0.0 ? nearest - now : 0.05);
      if (nearest != 0.0 && nearest - now < dt) dt = nearest - now;
      if (dt > 0.05) dt = 0.05;
      /* A thread that starts running signals a monitor in a timed wait only
         when the wait ends later than a quantum away (run_thread_once). While
         threads start often, keep the wait within a quantum, so they need not
         signal it; once they stop, back to the 50 ms backstop (SP_SYSMON_WARM).
         At SPINEL_WORKERS=1 main's own code is timed beside the runnable
         threads (above), and a thread main makes runnable signals no one: so
         while main runs, the monitor looks a quantum later at most, as its
         idle wait does. */
      if (!busy && dt > SP_PREEMPT_QUANTUM) {
        double last_run = 0.0;
        for (int i = 0; i < sp_active_workers; i++)
          if (g_wslot[i].active && wslot_since(i) > last_run) last_run = wslot_since(i);
        if ((last_run != 0.0 && now - last_run < SP_SYSMON_WARM) ||
            (g_worker_cap == 0 && SP_TH_STATE(&g_main_thread) == SP_TH_RUNNING))
          dt = SP_PREEMPT_QUANTUM;
      }
      /* A floor keeps a deadline already due from becoming a zero wait; it is
         small, since a sleep's lateness is whatever is left above it. */
      if (dt < 0.00001) dt = 0.00001;
      int tmo = (int)(dt * 1000.0); if (tmo < 1) tmo = 1;
#ifdef SP_EV_BACKEND
      /* Waiting in poll() is for descriptors, and it counts in whole
         milliseconds: with none to watch but its own wake pipe -- a program
         that has not waited on I/O yet -- the monitor rounded every deadline
         under 1 ms up to 1 ms, so `sleep 0.0002` took about 1.2 ms. Timing
         alone, it waits on the condvar too. */
      if (g_ev_fd > 0 || npf == 1) {
        /* The descriptors are in the WORKERS' sets now, so the monitor has no
           fds of its own to watch: it is a timer, for deadlines and the
           timeslice, and a condvar is the right thing to wait on. Its own wake
           pipe goes with them -- sp_sysmon_wake signals the condvar. */
        /* pthread_cond_timedwait's deadline is on CLOCK_REALTIME, and every
           other clock in this file is CLOCK_MONOTONIC. Handing it a monotonic
           stamp names a moment decades in the past, so the wait returns at
           once, every time: the monitor spun 1.5 MILLION turns where it should
           have taken 4,400, and burned a quarter of a core doing nothing. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)dt;
        ts.tv_nsec += (long)((dt - (double)(time_t)dt) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        SP_STAT_ADD(g_mon_polls, 1);
        SP_ATOMIC_STORE(&g_sysmon_idle, 2, __ATOMIC_SEQ_CST);
        /* published after the flag, which a worker reads after this: one that
           sees the time sees the condvar wait it signals */
        sysmon_wake_at_set(sp_monotonic_now() + dt);
        /* a deadline added since the heap was read and earlier than the one
           this wait is for: go round instead (see sp_sysmon_nudge). Compared
           with `nearest`, which is what was published, not with the end of
           the wait: that has a floor, and a deadline under the floor away
           read as new, so a monitor with sleepers due every few us never
           waited -- it went round 300,000 times in a run, on the lock. */
        double gn = nearest_get();
        if (!(gn != 0.0 && (nearest == 0.0 || gn < nearest)) && !sp_sysmon_missed_start(busy))
          pthread_cond_timedwait(&g_sysmon_cv, &g_sched_lock, &ts);
        sysmon_wake_at_set(0.0);
        SP_ATOMIC_STORE(&g_sysmon_idle, 0, __ATOMIC_SEQ_CST);
        continue;
      }
#endif
      /* Only the poll path needs the array; the degraded wait below is its
         fallback, not the event path's. Placed after it, the event path was
         never reached when the array had not been allocated -- which is the
         case whenever the walks are skipped, so a monitor that started with
         nothing parked nanosleeps 50ms a turn and delivers nothing. 102 turns
         of that is the five-second stall this cost. */
      if (!g_pfds) {   /* allocation failed: degrade to a plain timed wait */
        SCHED_UNLOCK();
        struct timespec req = { (time_t)dt, (long)((dt - (time_t)dt) * 1e9) };
        nanosleep(&req, NULL);
        SCHED_LOCK();
        continue;
      }
      g_pfds[0].fd = g_sysmon_pipe[0]; g_pfds[0].events = POLLIN; g_pfds[0].revents = 0;
      sysmon_wake_at_set(sp_monotonic_now() + tmo / 1000.0);
      if (sp_sysmon_missed_start(busy)) { sysmon_wake_at_set(0.0); continue; }
      SCHED_UNLOCK();
      int pr = poll(g_pfds, (nfds_t)npf, tmo);
      SCHED_LOCK();
      sysmon_wake_at_set(0.0);
      SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, npf);
      if (g_pfds[0].revents & POLLIN) {   /* drain the wake pipe */
        char buf[64]; while (read(g_sysmon_pipe[0], buf, sizeof buf) > 0) {}
      }
      if (pr > 0) {
        for (int i = 1; i < npf; i++) {
          if (!g_pfds[i].revents) continue;
          SP_STAT_ADD(g_mon_readied, 1);
          sp_thread *t = g_pths[i];
          if (t->wait_head != &g_io_waiters) continue;   /* unparked meanwhile (e.g. #kill, a close) */
          io_waiter_ready(t, g_pfds[i].revents);
        }
      }
    }
  }
  SCHED_UNLOCK();
  return NULL;
}

static int  sp_sched_unpark(sp_thread *t);   /* below */

/* Kernel#sleep, Thread.stop and Mutex#sleep park here: on a list of their own, with a
   timer unless seconds < 0 (until #wakeup). Answers 1 when Thread#wakeup
   ended the sleep early, 0 when the time ran out, -1 when its timer could
   not be allocated (the caller raises, after any cleanup of its own). */
static int sp_sched_sleep_park(double seconds) {
  if (!g_sysmon_started) {   /* the monitor was not started: plain blocking sleep */
    /* nothing else could wake us; a trap delivered meanwhile runs */
    if (seconds < 0.0) for (;;) { pause(); sp_sig_run_if_pending(); }
    struct timespec req; req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) req.tv_nsec = 0; if (req.tv_nsec >= 1000000000L) req.tv_nsec = 999999999L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) sp_sig_run_if_pending();
    return 0;
  }
  sp_thread *self = g_current;
  /* (#8702) On a list of its own under its own lock (sleep_link), with the
     timer under the timer lock: no scheduler lock. A pending #kill/#raise is checked, and the
     sleep published, with the thread's inject slot held, which is the lock
     sp_thread_deliver publishes under before it reads the state: one that
     arrived while we were running fires now, not after the full sleep, and one
     that arrives after finds us BLOCKED on the list. The timer is scheduled
     once we are on the list, so firing it early finds us there. */
  wl_lock(&self->sleep_wl);
  sp_Fiber *inj = sp_thread_inject_fiber(self);
  int held = 0;
  if (inj) {
    int ik = sp_fiber_inject_hold(inj);
    if (ik) {
      sp_fiber_inject_release(inj, ik);
      wl_unlock(&self->sleep_wl);
      sp_fiber_fire_inject_if_pending();   /* raises; does not return (a deferred one is left pending) */
      wl_lock(&self->sleep_wl);
    }
    else held = 1;
  }
  NTIMED_INC();   /* before it stops running */
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);   /* on a list, still on its stack */
  self->woken = 0;
  sleep_link(self);
  if (held) sp_fiber_inject_release(inj, 0);
  int nudge = 0;
  if (seconds >= 0.0) {
    uint64_t tm = sp_timer_schedule(self, sp_monotonic_now() + seconds, SP_TIMER_SLEEP, &nudge);
    if (!tm) {
      sleep_unlink(self);
      SP_ATOMIC_STORE(&self->park, SP_PK_RUN, __ATOMIC_SEQ_CST);
      SP_TH_SET_STATE(self, SP_TH_RUNNING);
      wl_unlock(&self->sleep_wl);
      NTIMED_DEC();
      return -1;
    }
    SP_ATOMIC_STORE(&self->wait_timer, tm, __ATOMIC_RELEASE);
  }
  wl_unlock(&self->sleep_wl);
  sp_sysmon_nudge(nudge);
  int woken;
  if (self == &g_main_thread) {
    SCHED_LOCK();
    sp_sched_pump(NULL, 1);   /* main waits (and pumps at N=1) until the monitor wakes it */
    SCHED_UNLOCK();
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    NTIMED_DEC();   /* running again */
    woken = self->woken;
    sp_fiber_fire_inject_if_pending();   /* a #raise delivered while sleeping */
  }
  else {
    /* Same exception-context snapshot as sp_sched_block: the symmetric transfer
       clobbers our handler stack on resume. */
    void *exc_snap = sp_snap_take();
    sp_exc_ctx_save(exc_snap);
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_snap_give(exc_snap);
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    NTIMED_DEC();   /* running again */
    woken = self->woken;
    sp_fiber_fire_inject_if_pending();   /* a #kill/#raise delivered while sleeping */
  }
  return woken;
}

static void sp_sched_sleep_park_or_raise(double seconds) {
  if (sp_sched_sleep_park(seconds) < 0) sp_raise_cls("NoMemoryError", "failed to schedule thread timer");
}

void sp_sched_sleep(double seconds) {
  if (!(seconds > 0.0)) return;
  sp_sched_sleep_park_or_raise(seconds);
}

void sp_sched_sleep_forever(void) { sp_sched_sleep_park_or_raise(-1.0); }

void sp_Thread_stop(void) {
  if (sp_Thread_list_count() <= 1)
    sp_raise_cls("ThreadError", "stopping only thread\n\tnote: use sleep to stop forever");
  sp_sched_sleep_park_or_raise(-1.0);
}

/* #wakeup ends a sleep (Kernel#sleep, Thread.stop, Mutex#sleep) early. A thread
   blocked on anything else is left alone, and one that is running loses the
   wakeup, as in CRuby. */
sp_thread *sp_Thread_wakeup(sp_thread *t) {
  if (SP_TH_STATE(t) == SP_TH_DEAD) sp_raise_cls("ThreadError", "killed thread");
  /* a sleep is under the thread's own lock: ended under it, woken after */
  wl_lock(&t->sleep_wl);
  int asleep = t->wait_head == &t->sleep_slot;
  if (asleep) { t->woken = 1; sleep_unlink(t); }
  wl_unlock(&t->sleep_wl);
  if (asleep) sched_wake_unlinked(t);   /* its timer cancelled, and runnable */
  return t;
}

sp_thread *sp_Thread_run(sp_thread *t) {
  sp_Thread_wakeup(t);
  sp_Thread_pass();
  return t;
}

sp_bool sp_Thread_stop_p(sp_thread *t) {
  SCHED_LOCK();
  sp_bool r = SP_TH_STATE(t) == SP_TH_DEAD || SP_TH_STATE(t) == SP_TH_BLOCKED;
  SCHED_UNLOCK();
  return r;
}

/* Mutex#sleep: unlock, sleep until the timeout or a #wakeup, lock again.
   nil when the timeout ran out, else the whole seconds slept. A #kill or
   #raise waits for the lock too, as CRuby re-locks before it unwinds. */
sp_RbVal sp_Mutex_sleep(sp_mutex *m, int has_timeout, double timeout) {
  if (has_timeout && timeout != timeout) sp_raise_cls("RangeError", "NaN out of Time range");
  if (has_timeout && timeout < 0.0) sp_raise_cls("ArgumentError", "time interval must not be negative");
  if (!has_timeout) timeout = -1.0;
  sp_Mutex_unlock(m);   /* raises ThreadError when we don't hold it */
  double t0 = sp_monotonic_now();
  /* a #raise or #kill is held until we own the mutex again, which the
     contended lock path would otherwise fire before we get it */
  sp_fiber_defer_inject();
  int woken = sp_sched_sleep_park(timeout);
  sp_Mutex_lock(m);
  sp_fiber_undefer_inject();
  if (woken < 0) sp_raise_cls("NoMemoryError", "failed to schedule thread timer");
  sp_fiber_fire_inject_if_pending();
  if (!woken && has_timeout) return sp_box_nil();
  return sp_box_int((sp_int)(sp_monotonic_now() - t0 + 0.5));
}

int sp_sched_wait_io(int fd, short events) {
  return sp_sched_wait_io_timeout(fd, events, -1.0);
}

/* One descriptor (set == NULL) or a set of n: the park is the same, the
   registration is per descriptor. */
static int sp_poll_plain(struct pollfd *pp, nfds_t np, double timeout_s) {
  if (timeout_s >= 0.0) {
    double until = sp_monotonic_now() + timeout_s;
    for (;;) {
      double left = until - sp_monotonic_now();
      int ms = left > 0.0 ? (int)(left * 1000.0) + 1 : 0;
      int pr = poll(pp, np, ms);
      if (pr > 0) return 1; if (pr == 0) return 0;
      if (errno == EINTR) { sp_sig_run_if_pending(); continue; }
      return 0;
    }
  }
  for (;;) {
    int pr = poll(pp, np, 1000);
    if (pr > 0) return 1;
    if (pr < 0 && errno == EINTR) sp_sig_run_if_pending();
    else if (pr != 0) return 0;
  }
}
static int sp_sched_wait_io_impl(int fd, short events, struct pollfd *set, int n, double timeout_s,
                                 const unsigned char *cancel) {
  if ((set ? n <= 0 : fd < 0) || !g_sysmon_started) {   /* no monitor: plain blocking poll */
    struct pollfd pf; pf.fd = fd; pf.events = events; pf.revents = 0;
    return sp_poll_plain(set ? set : &pf, set ? (nfds_t)n : 1, timeout_s);
  }
  sp_thread *self = g_current;
#ifdef SP_EV_BACKEND
  /* (#8702) With the event backend the park is the home worker's business
     (see NIO_INC): it takes the worker's I/O lock, and a deadline the timer
     lock. The worker's set is brought up under the scheduler lock, once.
     Lock order: the scheduler lock, io_wl, then the timer lock. */
  int home = sp_ev_home_of(self);
  int hfd = SP_ATOMIC_LOAD(&g_wslot[home].evfd, __ATOMIC_SEQ_CST);
  if (hfd <= 0 && hfd != -2) {
    SCHED_LOCK(); (void)sp_ev_worker_up(home); SCHED_UNLOCK();
  }
  if (SP_ATOMIC_LOAD(&g_wslot[home].evfd, __ATOMIC_SEQ_CST) > 0) {
    int timed = timeout_s >= 0.0;
    sp_wlock *wl = &g_wslot[home].io_wl;
    wl_lock(wl);
    /* Same pending-inject check as sp_sched_block. */
    if (sp_thread_inject_pending(self)) {
      wl_unlock(wl);
      sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    }
    /* A close from another thread that landed between the caller's readiness
       probe and this lock: the handle's closed flag is written before
       sp_sched_ev_forget takes the worker's io_wl, so a park that takes it
       afterwards sees the flag here, and one that took it first is readied by
       the forget itself (#4546). Sequentially consistent: a park that has
       just brought its worker's set up must see the flag, or the forget must
       see the set (see sp_sched_ev_forget). */
    if (cancel && SP_ATOMIC_LOAD(cancel, __ATOMIC_SEQ_CST)) { wl_unlock(wl); return 1; }
    sp_ev_waiter *es = NULL;
    if (set) {
      es = (sp_ev_waiter *)malloc(sizeof(sp_ev_waiter) * (size_t)n);
      if (!es) { wl_unlock(wl); return sp_poll_plain(set, (nfds_t)n, timeout_s); }
      for (int i = 0; i < n; i++) { es[i].t = self; es[i].idx = i; es[i].next = NULL; }
    }
    /* Checked again with the thread's inject slot held until it is published
       BLOCKED: sp_thread_deliver sets the inject and reads the state with the
       slot held, and without the scheduler lock here a #kill or #raise could
       land between the check above and the publication, find the thread
       running, and leave it to park with no deadline. */
    sp_Fiber *inj = sp_thread_inject_fiber(self);
    int ik = inj ? sp_fiber_inject_hold(inj) : 0;
    if (ik) {
      sp_fiber_inject_release(inj, ik);
      free(es); wl_unlock(wl);
      sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    }
    if (set) { self->io_fd = -1; self->io_events = 0; self->io_set = set; self->io_nset = n; self->ev_set = es; }
    else { self->io_fd = fd; self->io_events = events; }
    self->io_revents = 0;
    SP_TH_SET_STATE(self, SP_TH_BLOCKED);
    SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);   /* on a list, still on its stack */
    wio_link(home, self);
    if (inj) sp_fiber_inject_release(inj, 0);
    /* the deadline once it is on the list, so a timer that fires at once
       finds it there; without one it waits in poll() as before */
    int nudge = 0;
    if (timed) {
      uint64_t tm = sp_timer_schedule(self, sp_monotonic_now() + timeout_s, SP_TIMER_IO, &nudge);
      if (!tm) {
        wio_unlink(home, self);
        SP_ATOMIC_STORE(&self->park, SP_PK_RUN, __ATOMIC_SEQ_CST);
        SP_TH_SET_STATE(self, SP_TH_RUNNING);
        self->io_fd = -1;
        if (set) { self->ev_set = NULL; self->io_set = NULL; self->io_nset = 0; }
        free(es); wl_unlock(wl);
        return sp_poll_plain(set ? set : &(struct pollfd){.fd = fd, .events = events},
                             set ? (nfds_t)n : 1, timeout_s);
      }
      SP_ATOMIC_STORE(&self->wait_timer, tm, __ATOMIC_RELEASE);
    }
    SP_STAT_ADD(g_mon_regs, 1);
    if (set) { for (int i = 0; i < n; i++) (void)sp_ev_park_entry_w(home, &self->ev_set[i], set[i].fd); }
    else { self->ev0.t = self; self->ev0.idx = -1; (void)sp_ev_park_entry_w(home, &self->ev0, fd); }
    wl_unlock(wl);
    /* The kernel holds the interest set, so the monitor needs a word only
       about the deadline, when it is earlier than the one it waits for (an
       idle monitor waits for none: see sp_sysmon_nudge). */
    sp_sysmon_nudge(nudge);
    if (self == &g_main_thread) {
      SCHED_LOCK();
      sp_sched_pump(NULL, 1);   /* main waits (and pumps at N=1) until its worker readies it */
      SCHED_UNLOCK();
    }
    else {
      /* Same exception-context snapshot as sp_sched_block/sleep: the symmetric
         transfer clobbers our handler stack on resume. */
      void *exc_snap = sp_snap_take();
      sp_exc_ctx_save(exc_snap);
      sp_sched_switch_out();
      sp_exc_ctx_load(exc_snap);
      sp_snap_give(exc_snap);
    }
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    int rev = self->io_revents; self->io_revents = 0; self->io_fd = -1;
    /* the set entries are off every list (whoever unparked us dropped them);
       released before an inject can raise past this frame */
    if (set) { free(self->ev_set); self->ev_set = NULL; self->io_set = NULL; self->io_nset = 0; }
    sp_fiber_fire_inject_if_pending();   /* a #kill/#raise delivered while waiting on I/O */
    return rev ? 1 : 0;
  }
#endif
  /* Without the event backend: the monitor polls the waiters on g_io_waiters,
     under the scheduler lock. A close visits that list once this is up, and
     it goes up before the closed flag is read below (see sp_sched_ev_forget). */
#ifdef SP_THREADS
  SP_ATOMIC_STORE(&g_io_polled, 1, __ATOMIC_SEQ_CST);
#endif
  SCHED_LOCK();
  /* Same pending-inject check as sp_sched_block. */
  if (sp_thread_inject_pending(self)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    SCHED_LOCK();
  }
  /* A close from another thread that landed between the caller's readiness
     probe and this lock. The handle's closed flag is written before
     sp_sched_ev_forget takes the lock, so a park that takes it afterwards
     sees the flag here, and one that took it first is readied by the forget
     itself. Without this the registration went in after the forget (or the
     kernel dropped it at the close that followed) and a read with no
     deadline waited forever: the #4546 test hung on one macOS run in three. */
  if (cancel && SP_ATOMIC_LOAD(cancel, __ATOMIC_SEQ_CST)) { SCHED_UNLOCK(); return 1; }
  sp_ev_waiter *es = NULL;
  if (set) {
    es = (sp_ev_waiter *)malloc(sizeof(sp_ev_waiter) * (size_t)n);
    if (!es) { SCHED_UNLOCK(); return sp_poll_plain(set, (nfds_t)n, timeout_s); }
    for (int i = 0; i < n; i++) { es[i].t = self; es[i].idx = i; es[i].next = NULL; }
  }
  uint64_t ptm = 0;
  if (timeout_s >= 0.0 &&
      !(ptm = sp_timer_schedule(self, sp_monotonic_now() + timeout_s, SP_TIMER_IO, NULL))) {
    free(es);
    SCHED_UNLOCK();
    return sp_poll_plain(set ? set : &(struct pollfd){.fd = fd, .events = events},
                         set ? (nfds_t)n : 1, timeout_s);
  }
  if (set) { self->io_fd = -1; self->io_events = 0; self->io_set = set; self->io_nset = n; self->ev_set = es; }
  else { self->io_fd = fd; self->io_events = events; }
  self->io_revents = 0;
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);   /* on a list, still on its stack */
  io_waiters_link(self);
  if (ptm) SP_ATOMIC_STORE(&self->wait_timer, ptm, __ATOMIC_RELEASE);
  SP_STAT_ADD(g_mon_regs, 1);
  sp_sysmon_wake();   /* let the monitor rebuild its poll set */

  if (self == &g_main_thread) {
    sp_sched_pump(NULL, 1);   /* main waits (and pumps at N=1) until the monitor wakes it */
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    int rev = self->io_revents; self->io_revents = 0; self->io_fd = -1;
    if (set) { free(self->ev_set); self->ev_set = NULL; self->io_set = NULL; self->io_nset = 0; }
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* a #raise delivered while waiting on I/O */
    return rev ? 1 : 0;
  }
  /* Same exception-context snapshot as sp_sched_block/sleep: the symmetric
     transfer clobbers our handler stack on resume. */
  void *exc_snap = sp_snap_take();
  sp_exc_ctx_save(exc_snap);
  SCHED_UNLOCK();
  sp_sched_switch_out();
  sp_exc_ctx_load(exc_snap);
  sp_snap_give(exc_snap);
  SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    int rev = self->io_revents; self->io_revents = 0; self->io_fd = -1;
  /* the set entries are off every list (whoever unparked us dropped them);
     released before an inject can raise past this frame */
  if (set) { free(self->ev_set); self->ev_set = NULL; self->io_set = NULL; self->io_nset = 0; }
  sp_fiber_fire_inject_if_pending();   /* a #kill/#raise delivered while waiting on I/O */
  return rev ? 1 : 0;
}
int sp_sched_wait_io_timeout(int fd, short events, double timeout_s) {
  return sp_sched_wait_io_impl(fd, events, NULL, 0, timeout_s, NULL);
}
int sp_sched_wait_io_unless(int fd, short events, const unsigned char *cancel) {
  return sp_sched_wait_io_impl(fd, events, NULL, 0, -1.0, cancel);
}
int sp_sched_wait_io_timeout_unless(int fd, short events, double timeout_s, const unsigned char *cancel) {
  return sp_sched_wait_io_impl(fd, events, NULL, 0, timeout_s, cancel);
}
int sp_sched_wait_io_set(struct pollfd *set, int n, double timeout_s) {
  if (n == 1) return sp_sched_wait_io_impl(set[0].fd, set[0].events, NULL, 0, timeout_s, NULL);
  return sp_sched_wait_io_impl(-1, 0, set, n, timeout_s, NULL);
}

/* A helper worker: adopt its native stack as a per-worker root fiber, then pull
   runnable green threads off the GRQ forever. It parks at the GC barrier when a
   collection is in progress and exits when main signals shutdown at drain. */
/* (#8702) run what is queued on this worker without the scheduler
   lock, for as long as there is some (PRE/POST: the scheduler lock NOT held).
   A thread taken off the queue stays counted runnable until run_thread_once
   counts it running, in one change (runq_take): main's pump reads both to
   decide that nothing can wake it. Every 61st pick, after a thread that
   ran out its quantum, and when a descriptor of a thread pinned here is
   ready, it returns for one turn of the locked loop, which drains this
   worker's readiness set and gives the global queue its turn. Answers
   SP_LOCAL_TURN then, and otherwise whether it ran anything. */
#define SP_LOCAL_TURN 2
static int sp_worker_run_local(int wid) {
  RQ_LOCK(wid); sp_thread *ft = runq_take(&g_lrq[wid]); RQ_UNLOCK(wid);
  int ran = ft != NULL;
  while (ft) {
    g_run_taken = 1;
    g_run_preempted = 0;
    run_thread_once(ft);
    ft = NULL;
    if (SP_ATOMIC_LOAD(&g_stw_active, __ATOMIC_SEQ_CST) || SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)) break;
    if ((++g_pick_tick % 61u) == 0) { ran = SP_LOCAL_TURN; break; }
    if (g_run_preempted) { ran = SP_LOCAL_TURN; break; }
#ifdef SP_EV_BACKEND
    /* the I/O of threads pinned here waits in this worker's own set; readying
       it takes only the worker's own lock (sp_ev_drain), so it is done here,
       between threads, when it is due (sp_ev_drain_due), and what it readies
       lands on this worker's queue */
    if (g_wslot[wid].evfd > 0 && sp_ev_drain_due(wid)) (void)sp_ev_drain(wid);
#endif
    RQ_LOCK(wid); ft = runq_take(&g_lrq[wid]); RQ_UNLOCK(wid);
  }
  return ran;
}

/* (#8702) A worker's rounds without the scheduler lock. Once its own queue
   has had work, a worker runs it, drains its readiness set, steals an
   unstarted thread, and waits for more out of the world, all without the
   scheduler lock, round after round. It goes back to the locked loop only
   for what needs that lock: a collection, the exit, a thread on the global
   queue, and every 61st pick, which gives the global queue its turn. The
   locked loop took the lock three times a round -- after the drain, after
   the idle wait, and after running the queue -- and in a thread-per-
   connection server, where the workers go idle between requests, 64
   workers waited 14 cores' worth of time for it, to hold it for a few
   hundred nanoseconds each. */
enum { SP_UL_IN, SP_UL_OUT };   /* back to the locked loop in the world, or still out of it */
/* Does the locked loop have something to do? */
static int sp_ul_wants_lock(void) {
  return SP_ATOMIC_LOAD(&g_stw_active, __ATOMIC_SEQ_CST) || SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)
      || SP_ATOMIC_LOAD(&g_grq.head, __ATOMIC_ACQUIRE) != NULL;
}
/* Wait for work out of the world, without the scheduler lock, as the locked
   loop's idle wait does with it. Answers SP_UL_IN back in the world, or
   SP_UL_OUT still counted out, for the locked loop to bring it back
   (sp_out_leave_locked): a collection is up, or one left a sweep for it. */
static int sp_worker_idle_unlocked(int wid) {
  /* Out of the world, as sp_out_enter_locked: this worker's own roots are
     published and its fibers recorded for the mark before it counts itself
     out. A collector stores g_stw_active and then counts the workers out;
     this counts itself out and then reads g_stw_active, all sequentially
     consistent: the collector counts this worker, or this sees the
     collection, and tells a collector that may be waiting for the count
     under the lock it waits with. */
  sp_stw_publish_locked();   /* this worker's own state: no lock needed */
  { sp_Fiber *root = sp_fiber_worker_root();
    g_native_nfiber[wid] = 0;
    if (sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = sp_fiber_current;
    if (root && root != sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = root;
    SP_ATOMIC_STORE(&g_native_out[wid], (unsigned char)SP_OUT_IDLE, __ATOMIC_RELEASE); }
  int nout = SP_ATOMIC_FETCH_ADD(&g_nnative, 1, __ATOMIC_SEQ_CST) + 1;
  /* the last helper to go idle wakes a main waiting for nothing to be left,
     as sp_out_enter_locked does: under the lock that main waits under. Not
     when a wake has reached main since it last looked (g_main_waits): a
     join's wake has, and the lock here then met main taking it on waking. */
  if (nout >= SP_ATOMIC_LOAD(&sp_active_workers, __ATOMIC_ACQUIRE) - 1 && SP_ATOMIC_LOAD(&g_main_waits, __ATOMIC_SEQ_CST)) {
    SCHED_LOCK(); sp_sched_signal_if_quiescent(); SCHED_UNLOCK();
  }
  if (SP_ATOMIC_LOAD(&g_stw_active, __ATOMIC_SEQ_CST)) {
    SCHED_LOCK();
    if (g_stw_active && g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) >= sp_active_workers - 1)
      pthread_cond_signal(&g_stw_request);
    SCHED_UNLOCK();
    return SP_UL_OUT;
  }
  /* Parked, then the queues once more: a thread queued for any worker is
     handed by its pusher to a parked worker (sched_wake_idle_helper), which
     fences between the two as this does. One queued here wakes this worker
     itself: a kick is a byte in a pipe the wait below watches, and the
     condvar's flag is set under the queue's lock that a push holds. */
  SP_ATOMIC_STORE(&g_wslot[wid].parked, 1, __ATOMIC_SEQ_CST);
  SP_ATOMIC_STORE(&g_wslot[wid].wake_sent, 0, __ATOMIC_RELAXED);
  SP_ATOMIC_FENCE(__ATOMIC_SEQ_CST);
  int work = SP_ATOMIC_LOAD(&g_lrq[wid].head, __ATOMIC_ACQUIRE) != NULL
          || SP_ATOMIC_LOAD(&g_grq.head, __ATOMIC_ACQUIRE) != NULL
          || SP_ATOMIC_LOAD(&g_lrq_unstarted, __ATOMIC_SEQ_CST) != 0
          || SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED);
  if (!work) {
#ifdef SP_EV_BACKEND
    if (g_wslot[wid].evfd > 0) {
      sp_ev_ready evs[64];
      sp_ev_arm_kick(wid);
      int en = sp_ev_backend_wait(g_wslot[wid].evfd, evs, 64, SP_EV_BACKSTOP_MS);
      if (en == 0) SP_STAT_ADD(g_ev_backstop, 1);
      SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, en > 0 ? en : 0);
      if (en > 0) (void)sp_ev_handle(wid, evs, en);
    }
    else
#endif
    {
      RQ_LOCK(wid);
      /* shutdown is read again under the queue's lock: the drain stores it
         and then kicks this worker under that lock, and a kick reaches only
         a worker whose `idle` is up. Read once above, before the lock, a
         drain in between found `idle` down, and this worker then waited for
         good while main joined it (a program that printed all it had to,
         then never exited: gc-minor-test on CI). So is every other reason
         a kick was sent: a waker that chose this worker for a thread on the
         global queue or one to steal raises `wake_sent` before it kicks
         (sched_wake_idle_helper), and a finished collection leaves its
         sweeps unclaimed before it kicks the idle workers to take their own
         (sched_wake_all_workers). Either kick, sent in the same window, was
         lost, and the work waited for some later wake. */
      if (!g_lrq[wid].head && !SP_ATOMIC_LOAD(&g_shutdown, __ATOMIC_RELAXED)
          && !SP_ATOMIC_LOAD(&g_wslot[wid].wake_sent, __ATOMIC_RELAXED)
          && !SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED)) {
        g_wslot[wid].idle = 1;
        pthread_cond_wait(&g_wslot[wid].cv, &g_rq_lock[wid].m);   /* woken for work meant for us, or shutdown */
        g_wslot[wid].idle = 0;
      }
      RQ_UNLOCK(wid);
    }
  }
  SP_ATOMIC_STORE(&g_wslot[wid].parked, 0, __ATOMIC_RELAXED);
  /* Back in the world with atomics alone, as the locked loop's condvar wait
     comes back: the count drops before stop-the-world is read, and a
     collection seen then puts this worker back out and through the locked
     loop, so a collector that counted it out never sees it run. A sweep a
     collection left for it is taken there too. g_native_out stays up until
     no collection is seen: a collector that counted this worker out marks
     the fibers it recorded by that flag, so it must not find it down. */
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) return SP_UL_OUT;
  SP_ATOMIC_FETCH_SUB(&g_nnative, 1, __ATOMIC_SEQ_CST);
  if (SP_ATOMIC_LOAD(&g_stw_active, __ATOMIC_SEQ_CST)) {
    SP_ATOMIC_FETCH_ADD(&g_nnative, 1, __ATOMIC_SEQ_CST);
    return SP_UL_OUT;
  }
  SP_ATOMIC_STORE(&g_native_out[wid], 0, __ATOMIC_RELEASE);
  g_native_nfiber[wid] = 0;
  return SP_UL_IN;
}
/* The rounds. PRE/POST: the scheduler lock not held. */
static int sp_worker_unlocked(int wid) {
  for (;;) {
    if (SP_ATOMIC_LOAD(&g_lrq[wid].head, __ATOMIC_ACQUIRE) && sp_worker_run_local(wid) == SP_LOCAL_TURN) return SP_UL_IN;
    if (sp_ul_wants_lock()) return SP_UL_IN;
#ifdef SP_EV_BACKEND
    if (g_wslot[wid].evfd > 0 && sp_ev_drain(wid) > 0) continue;
#endif
    if (SP_ATOMIC_LOAD(&g_lrq[wid].head, __ATOMIC_ACQUIRE)) continue;
    /* An unstarted thread on a busier worker's queue, taken as
       sp_worker_run_local takes its own */
    sp_thread *t = sched_steal(wid, 1);
    if (t) { g_run_taken = 1; run_thread_once(t); continue; }
    int r = sp_worker_idle_unlocked(wid);
    if (r != SP_UL_IN) return r;
  }
}

static void *sp_worker_main(void *arg) {
  int wid = (int)(intptr_t)arg;
  sp_worker_id = wid;          /* TLS: read only by this worker */
  cnt_claim(wid);              /* its run counts (cnt_add) */
  sp_fiber_worker_init();
  SCHED_LOCK();
  int locked_turn = 0;   /* the fast path asked for one turn of this loop */
  g_wslot[wid].tid = pthread_self();   /* publish under the lock; the monitor reads it there */
  g_wslot[wid].active = 1;
  for (;;) {
    if (g_stw_active) { sp_stw_park_locked(); continue; }
    if (g_shutdown) break;
    /* (#8702) the fast path. Run what is queued on this worker
       without the scheduler lock for as long as there is some. The lock is
       taken again to look anywhere else, to go idle, to park for a
       collection, and every 61st pick, which gives the global queue its turn
       and drains this worker's readiness set (the I/O of threads pinned
       here). */
    /* A thread taken off the queue stays counted runnable until
       run_thread_once counts it running, in one change (runq_take). */
    /* A turn the fast path asked for is taken before going back to it: with
       this worker's queue never empty (a thread that does not block), the
       loop went straight back in, and neither the global queue nor the
       readiness set had a turn. At one helper, a thread spawned before the
       helper started waited on the global queue for good. */
    if (!locked_turn) {
      RQ_LOCK(wid); int have = g_lrq[wid].head != NULL; RQ_UNLOCK(wid);
      if (have) {
        SCHED_UNLOCK();
        int r = sp_worker_unlocked(wid);
        SCHED_LOCK();
        if (r == SP_UL_OUT) sp_out_leave_locked(wid);
        locked_turn = 1;   /* it came back for the lock: take a turn with it */
        continue;
      }
    }
    locked_turn = 0;
#ifdef SP_EV_BACKEND
    /* Drain what is ready before picking, when it is due (sp_ev_drain_due:
       with nothing queued here, and otherwise every few threads or a
       millisecond). A worker with runnable work never reaches the blocking
       wait below, and its OWN set is the only place its parked threads are
       delivered from -- so without this a busy worker starves them until it
       happens to idle. That showed as a ping-pong that ran at full speed on
       one worker and fell to a fifth of it on four, run to run. */
    if (g_wslot[wid].evfd > 0 && sp_ev_drain_due(wid)) sp_ev_worker_wait(wid, 0);
#endif
    sp_thread *t = sched_pick(wid);   /* own queue, then global, then steal */
    if (t) { run_thread_once(t); continue; }  /* run_thread_once signals quiescence on the last one */
#ifdef SP_EV_BACKEND
    /* Nothing to run: wait on THIS worker's readiness set, so a descriptor
       belonging to a thread pinned here wakes the worker that can run it --
       no monitor round trip and no condvar hand-off (#4306). The kick pipe in
       the same set is how a stop-the-world, a shutdown, or work enqueued for
       us gets through. The timeout is a backstop, not the mechanism. */
    if (g_wslot[wid].evfd > 0) {
      sp_ev_worker_wait(wid, SP_EV_BACKSTOP_MS);
      continue;
    }
#endif
    /* `idle` is set under the lock the enqueue also holds, so a wake issued
       between our sched_pick and this wait cannot be lost. */
    SP_ATOMIC_STORE(&g_wslot[wid].parked, 1, __ATOMIC_SEQ_CST); SP_ATOMIC_STORE(&g_wslot[wid].wake_sent, 0, __ATOMIC_RELAXED);
    sp_out_enter_locked(wid, SP_OUT_IDLE);
    /* `idle` is set and read under this worker's run-queue lock, which a
       push holds too, and the wait releases that lock, so a push between our
       look at the queue and the wait still finds `idle` set. */
    RQ_LOCK(wid);
    if (!g_lrq[wid].head) {
      g_wslot[wid].idle = 1;
      SCHED_UNLOCK();
      pthread_cond_wait(&g_wslot[wid].cv, &g_rq_lock[wid].m);   /* woken for work meant for us, or shutdown */
      g_wslot[wid].idle = 0;
      int work = g_lrq[wid].head != NULL;
      RQ_UNLOCK(wid);
      /* woken for a thread pinned here, with no collection up and no
         sweep left for us: back in the world with atomics alone, then run it.
         The count drops before stop-the-world is read, and a collection seen
         then puts us back out and through the locked path, so a collector
         that counted us out never sees us run. g_native_out stays up until
         no collection is seen: a collector that counted us out marks the
         fibers we recorded by that flag. */
      if (work && !SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED)) {
        SP_ATOMIC_FETCH_SUB(&g_nnative, 1, __ATOMIC_SEQ_CST);
        if (!SP_ATOMIC_LOAD(&g_stw_active, __ATOMIC_SEQ_CST)) {
          SP_ATOMIC_STORE(&g_native_out[wid], 0, __ATOMIC_RELEASE);
          g_native_nfiber[wid] = 0;
          SP_ATOMIC_STORE(&g_wslot[wid].parked, 0, __ATOMIC_RELAXED);
          int r = sp_worker_unlocked(wid);
          SCHED_LOCK();
          if (r == SP_UL_OUT) sp_out_leave_locked(wid);
          locked_turn = 1;
          continue;
        }
        SP_ATOMIC_FETCH_ADD(&g_nnative, 1, __ATOMIC_SEQ_CST);
      }
      SCHED_LOCK();
    }
    else RQ_UNLOCK(wid);
    sp_out_leave_locked(wid);
    SP_ATOMIC_STORE(&g_wslot[wid].parked, 0, __ATOMIC_RELAXED);
  }
  SCHED_UNLOCK();
  return NULL;
}

#ifndef NSIG
#define NSIG 65
#endif

/* Resolve SPINEL_PREEMPT_SIGNAL to the signal the monitor sends. Accepts a number
   (so real-time signals work, e.g. `kill -l SIGRTMIN`) or a name with or without
   the SIG prefix (URG/USR1/USR2/IO/WINCH). The default, SIGURG, is chosen because
   real programs essentially never use it (its nominal job is TCP out-of-band data)
   and its default disposition is "ignore", so it is safe to repurpose; override it
   only if the program itself needs SIGURG, or wants a real-time signal. An
   unrecognized or uncatchable value warns and falls back to SIGURG. */
static int sp_resolve_preempt_signal(void) {
  const char *e = getenv("SPINEL_PREEMPT_SIGNAL");
  if (!e || !*e) return SIGURG;
  char *end;
  long n = strtol(e, &end, 10);
  if (*end == '\0') {
    if (n > 0 && n < NSIG) return (int)n;
  }
  else {
    const char *name = e;
    if (strncasecmp(name, "SIG", 3) == 0) name += 3;
    static const struct { const char *n; int s; } tab[] = {
      { "URG", SIGURG }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
      { "IO", SIGIO }, { "WINCH", SIGWINCH },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
      if (strcasecmp(name, tab[i].n) == 0) return tab[i].s;
  }
  fprintf(stderr, "spinel: ignoring unrecognized SPINEL_PREEMPT_SIGNAL=%s; using SIGURG\n", e);
  return SIGURG;
}

static int sp_sched_start_workers(void) {
  /* Spawn the monitor thread (scheduler timers) before any worker. Without it,
     timed waits cannot be woken by the deadline queue, so do not permit helper
     workers to run concurrently with the blocking fallback. */
  /* Fix the helper cap before spawning anything, so the monitor and helpers read
     it through the pthread_create happens-before edge (no lock needed). Helpers
     themselves are spawned on demand (sp_sched_maybe_grow), not here -- main
     stays worker 0 and starts as the only participant (sp_active_workers 1). */
  g_worker_cap = sp_worker_count();
  /* SPINEL_WORKERS=N counts every OS worker that runs Ruby code, main among
     them, as docs/thread.md says (#9840): N-1 helpers, and none at N=1,
     where main runs every green thread and the monitor asks it for their
     slices (sp_safepoint_preempt). The autodetect, with the variable unset,
     stays one helper per core beside main. */
  { const char *e = getenv("SPINEL_WORKERS"); if (e && *e) g_worker_cap--; }
  /* main is worker 0 and the last slot belongs to the sweeper threads */
  if (g_worker_cap > SP_MAX_WORKERS - 2) g_worker_cap = SP_MAX_WORKERS - 2;
  /* Install the preemption signal handler before any worker can be targeted.
     SA_RESTART so an in-flight library syscall resumes rather than failing with
     EINTR -- the yield itself is cooperative (at the next safepoint poll), the
     signal only nudges the worker there. Resolve and pin g_preempt_sig here,
     before the monitor reads it through the pthread_create edge. */
  g_preempt_sig = sp_resolve_preempt_signal();
  /* RUBY_THREAD_TIMESLICE as CRuby reads it: milliseconds, ignored unless
     positive. Fixed here, before the monitor and any helper start. */
  { const char *e = getenv("RUBY_THREAD_TIMESLICE");
    long ms = e ? strtol(e, NULL, 0) : 0;
    if (ms > 0) { g_quantum = ms / 1000.0; g_tick = g_quantum / 2 < 0.005 ? g_quantum / 2 : 0.005; } }
  { const char *e = getenv("SPINEL_WAKE_PREEMPT"); g_wake_preempt = e && strcmp(e, "1") == 0; }
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_handler = sp_preempt_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(g_preempt_sig, &sa, NULL) != 0) {   /* uncatchable signal: fall back */
    fprintf(stderr, "spinel: SPINEL_PREEMPT_SIGNAL=%d cannot be caught; using SIGURG\n", g_preempt_sig);
    g_preempt_sig = SIGURG;
    sigaction(g_preempt_sig, &sa, NULL);
  }
  /* Self-pipe so a thread registering for sleep/I/O can break the monitor out of
     poll() immediately. Both ends non-blocking: the writer never stalls on a full
     pipe (the bytes only signal, they are drained wholesale), the reader drains
     without blocking. Created before the monitor so it sees valid fds. */
  if (sp_io_make_pipe(g_sysmon_pipe) == 0) {
    for (int e = 0; e < 2; e++) { int fl = fcntl(g_sysmon_pipe[e], F_GETFL, 0); if (fl >= 0) fcntl(g_sysmon_pipe[e], F_SETFL, fl | O_NONBLOCK); }
  }
  else { g_sysmon_pipe[0] = g_sysmon_pipe[1] = -1; }
  if (pthread_create(&g_sysmon, NULL, sp_sysmon_main, NULL) == 0) {
    g_sysmon_started = 1;
    /* SPINEL_SCHED_STATS reports at any end of the program -- `exit`, an
       uncaught exception -- not only when main returns and drains */
    atexit(sp_sched_report_stats);
    return 1;
  }
  if (g_sysmon_pipe[0] >= 0) close(g_sysmon_pipe[0]);
  if (g_sysmon_pipe[1] >= 0) close(g_sysmon_pipe[1]);
  g_sysmon_pipe[0] = g_sysmon_pipe[1] = -1;
  return 0;
}

/* Create one helper worker (next id). PRE: g_sched_lock held, below the cap, and
   no collection in progress (sp_sched_maybe_grow enforces the last two). The id
   count (sp_active_workers) is bumped only after pthread_create succeeds, so the
   STW barrier never waits on a worker that failed to start. */
static void sp_sched_spawn_helper(void) {
  int wid = g_helpers_spawned + 1;
  if (wid > g_worker_cap || wid >= SP_MAX_WORKERS) return;
  g_wslot[wid].idle = 0;
  if (pthread_cond_init(&g_wslot[wid].cv, NULL) != 0) return;
  if (pthread_create(&g_worker_threads[wid], NULL, sp_worker_main, (void *)(intptr_t)wid) != 0) return;
  g_helpers_spawned = wid;
  SP_ATOMIC_STORE(&sp_active_workers, g_helpers_spawned + 1, __ATOMIC_RELEASE);   /* participants: main (0) + helpers; read without the lock by a stealing worker */
}

/* Grow the helper pool toward one execution worker per live green thread, capped
   at g_worker_cap. Called under the lock wherever a thread becomes runnable; a
   no-op once at the cap or while a collection is reading the participant count
   (we never change it mid-STW). This turns "SPINEL_WORKERS=N" into up to N real
   parallel execution threads while never starting a helper the workload has no
   runnable thread for -- a lone background thread brings up exactly one. */
static void sp_sched_maybe_grow(void) {
  if (!g_workers_started || g_stw_active || g_helpers_spawned >= g_worker_cap) return;
  int crq, cru; counts_approx(&crq, &cru);   /* a pass over the slots: only while the pool can still grow */
  int live = crq + cru;
  int want = live < g_worker_cap ? live : g_worker_cap;
  while (g_helpers_spawned < want) {
    int before = g_helpers_spawned;
    sp_sched_spawn_helper();
    if (g_helpers_spawned == before) break;   /* spawn failed: stop, main will cope */
  }
}
#endif

/* SPINEL_SCHED_STATS=1: what the monitor did, on the way out. A wake costs
   O(parked) three times over, so what sizes a deployment is how OFTEN the
   monitor turned and how BIG its poll set was each time -- and neither is
   visible from outside the process, which is what left a 6x gap between a
   standalone and a real server unexplained (#4317). */
static void sp_sched_report_stats(void) {
#ifdef SP_THREADS
  /* main's drain and the atexit hook both call it. From the hook (an
     `exit` before the drain) the monitor may still be running, so the
     counters are a best-effort snapshot -- read without the lock, which the
     exiting code may hold. */
  static int reported = 0;
  const char *e = getenv("SPINEL_SCHED_STATS");
  if (reported || !e || !*e || *e == '0') return;
  reported = 1;
  unsigned long long iters = SP_STAT_GET(g_mon_iters), polls = SP_STAT_GET(g_mon_polls),
                     pollfds = SP_STAT_GET(g_mon_pollfds), regs = SP_STAT_GET(g_mon_regs),
                     readied = SP_STAT_GET(g_mon_readied);
  double avg = polls ? (double)pollfds / (double)polls : 0.0;
  fprintf(stderr,
          "[sched] monitor: %llu turns, %llu polls, %.1f fds/poll avg, "
          "%llu fds total; %llu io parks registered, %llu waiters readied\n",
          iters, polls, avg, pollfds, regs, readied);
#ifdef SP_EV_BACKEND
  if (g_ev_fd >= 0 || SP_STAT_GET(g_ev_arms))
    fprintf(stderr, "[sched] events: %llu arms (%llu adds), %llu refused, "
                    "%llu deadline expiries, %llu backstop expiries\n",
            SP_STAT_GET(g_ev_arms), SP_STAT_GET(g_ev_adds), SP_STAT_GET(g_ev_lost),
            SP_STAT_GET(g_ev_timeouts), SP_STAT_GET(g_ev_backstop));
#endif
#endif
}

/* main() is finishing and has at_exit hooks to run: the same turn for the
   remaining runnable threads, with the helpers left up. A hook runs as CRuby's
   does, while the program's threads are alive, and a thread it starts needs a
   worker; sp_sched_drain ends them once the hooks are done. */
void sp_sched_drain_runnable(void) {
  if (g_current != &g_main_thread) return;
  SCHED_LOCK();
  sp_sched_pump(NULL, 2);
  SCHED_UNLOCK();
}

void sp_sched_drain(void) {
  /* main() is finishing: run remaining runnable threads so fire-and-forget side
     effects happen, then shut the helper workers down. Only the main thread
     drains. pump(NULL, 1) returns once the queue is empty and every helper is
     idle -- i.e. all runnable work is done (at N=1 it returns on an empty queue
     exactly as before). */
  if (g_current != &g_main_thread) return;
  SCHED_LOCK();
  sp_sched_pump(NULL, 2);   /* exit drain: runnable work only, not sleepers */
#ifdef SP_THREADS
  /* stored atomically: the sweeper and the trim thread read it without the
     sched lock (the workers and the monitor read it under it) */
  SP_ATOMIC_STORE(&g_shutdown, 1, __ATOMIC_RELAXED);
  sched_wake_all_workers(0);
  sp_sysmon_wake();   /* wake the monitor (idle or in poll) so it sees shutdown */
  int sysmon_running = g_sysmon_started;
  SCHED_UNLOCK();
  for (int i = 1; i < sp_active_workers; i++) pthread_join(g_worker_threads[i], NULL);
  if (sysmon_running) pthread_join(g_sysmon, NULL);
  sp_sched_report_stats();
  return;
#endif
  SCHED_UNLOCK();
}

/* ---- generic park / wake on a primitive wait list ---- */

/* Block the current green thread on `*waitlist` until a wake moves it back to
   runnable. The main thread pumps the scheduler (it cannot transfer away from
   root); a spawned thread transfers back to the scheduler hub. `defer_inject`
   keeps a condition-wait interruption pending until its caller reacquires m. */
static void sp_sched_block(sp_thread **waitlist, int defer_inject) {   /* PRE/POST: sched lock held */
  sp_thread *self = g_current;
  /* A #kill/#raise delivered while this thread was RUNNING left its inject
     pending on the fiber (sp_thread_deliver found no wait list to unpark).
     Parking now would sleep through it forever -- nobody will wake us. Fire it
     here instead (it raises out of the blocking call, ensures running). The
     deliver and this check both run under the sched lock, so no window. */
  if (sp_thread_inject_pending(self)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    SCHED_LOCK();
  }
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);   /* on a list, still on its stack */
  /* Append at the TAIL: sp_sched_wake_one takes the head, so the list is a
     FIFO and a waiter is served in the order it arrived. Pushed at the head
     it was a LIFO -- the newest waiter woke first -- and under sustained
     contention an early waiter never reached the front: campfire's DB-pool
     condvar and fragment-cache mutexes held requests for seconds at p99 (a
     2.9 s mutex wait, an 8 s condvar wait at 64 connections) while the median
     was 4 ms. The walk is O(waiters) under the scheduler lock, which is what
     one park already costs. */
  self->wait_head = waitlist;   /* so #kill/#raise can unlink it */
  SP_ATOMIC_STORE(&self->wait_lock, NULL, __ATOMIC_RELEASE);       /* this list is under the scheduler lock */
  if (self->repark_front) {   /* a mutex waiter that lost the race after its wake: keep its turn */
    self->repark_front = 0;
    self->wait_next = *waitlist; *waitlist = self;
  }
  else {
    self->wait_next = NULL;
    sp_thread **pp = waitlist; while (*pp) pp = &(*pp)->wait_next; *pp = self;
  }
  if (self == &g_main_thread) {
    for (;;) {
      sp_sched_pump(NULL, 1);   /* returns (lock held) once a waker marks main RUNNABLE */
      if (SP_TH_STATE(self) == SP_TH_RUNNING) break;
      if (sched_main_leave_deadlocked_wait(self)) {
        SCHED_UNLOCK();
        sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
      }
    }
    /* a #raise that woke main fires here, lock not held, as below */
    if (defer_inject) sp_fiber_defer_inject();
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
  else {
    /* The symmetric fiber transfer's exc bookkeeping clobbers this thread's
       handler stack when root resumes from the block, so snapshot it here and
       restore it on wake -- otherwise a #raise/#kill delivered while blocked
       would find an empty handler stack and escape unhandled. */
    void *exc_snap = sp_snap_take();
    sp_exc_ctx_save(exc_snap);
    if (defer_inject) sp_fiber_defer_inject();
    SCHED_UNLOCK();   /* drop the lock across the transfer (we run no metadata while parked) */
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_snap_give(exc_snap);
    /* resumed: a pending #kill/#raise fires here, in this thread's context (lock
       not held), so its ensure/rescue blocks unwind on its own stack. On the
       normal resume we re-take the lock so the caller continues holding it. */
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
}

/* (#8702) park on a list guarded by wait lock `wl` (held on
   entry and on return; the scheduler lock is not). The thread is linked and
   marked PARKING under `wl`, so a waker that unlinks it under `wl` either finds
   it PARKING (and its worker queues it once it is off its stack) or PARKED. If
   `release` is set (ConditionVariable#wait) that mutex is released after the
   thread is on the list and before it switches out: a #signal from whoever
   takes the mutex next finds it there. */
static void sp_sched_block_wl(sp_thread **waitlist, sp_wlock *wl, int defer_inject, sp_mutex *release) {
  sp_thread *self = g_current;
  /* A pending #kill/#raise is checked, and the park published, with the
     thread's inject slot held (sp_fiber_inject_hold). sp_thread_deliver sets
     the inject under the scheduler lock, which this does not take, and reads
     the state after it: checked under `wl` alone, a delivery could land
     between the check and the publication, find the thread running and leave
     the inject for it to notice, while it went on to park until the list's
     next wake -- forever, for a Queue#pop nothing will push to. Under the
     slot's lock it lands before the check, and the thread raises, or after
     the publication, and it is unparked. */
  sp_Fiber *inj = sp_thread_inject_fiber(self);
  int held = 0;
  if (inj) {
    int ik = sp_fiber_inject_hold(inj);
    if (ik) {
      sp_fiber_inject_release(inj, ik);
      wl_unlock(wl);
      sp_fiber_fire_inject_if_pending();   /* raises; does not return */
      wl_lock(wl);
    }
    else held = 1;
  }
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);
  self->wait_head = waitlist;
  SP_ATOMIC_STORE(&self->wait_lock, wl, __ATOMIC_RELEASE);
  if (self->repark_front) { self->repark_front = 0; self->wait_next = *waitlist; *waitlist = self; }
  else { self->wait_next = NULL; sp_thread **pp = waitlist; while (*pp) pp = &(*pp)->wait_next; *pp = self; }
  if (held) sp_fiber_inject_release(inj, 0);
  wl_unlock(wl);
  if (release) { SP_ATOMIC_STORE(&release->owner, NULL, __ATOMIC_SEQ_CST); sp_mutex_wake_waiter(release); }
  if (self == &g_main_thread) {
    SCHED_LOCK();
    for (;;) {
      sp_sched_pump(NULL, 1);   /* returns once a waker marks main RUNNABLE */
      if (SP_TH_STATE(self) == SP_TH_RUNNING) break;
      if (sched_main_leave_deadlocked_wait(self)) {
        SCHED_UNLOCK();
        sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
      }
    }
    SCHED_UNLOCK();
    if (defer_inject) sp_fiber_defer_inject();
    sp_fiber_fire_inject_if_pending();   /* a #raise that woke main */
  }
  else {
    void *exc_snap = sp_snap_take();
    sp_exc_ctx_save(exc_snap);
    if (defer_inject) sp_fiber_defer_inject();
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_snap_give(exc_snap);
    sp_fiber_fire_inject_if_pending();
  }
  wl_lock(wl);
}

#ifdef SP_THREADS
/* (#8702) A timed wait parks on its list under the list's own wait lock `wl`
   and schedules its timer under the timer lock, with no scheduler lock held
   on entry or on return. If mutex is non-NULL, it is checked and released as
   part of parking (ConditionVariable#wait); interruption stays pending until
   the caller reacquires it. Other waiters such as Queue#pop pass NULL.
   Returns -1 if timer storage could not be allocated, 0 if the deadline
   expired, or 1 if another event woke the thread. A caller that looked at what
   it waits for before passes that look as `no_wait`: it is taken again under
   `wl`, and a true answer there ends the wait at once (woken) rather than at
   the deadline -- a join's target dead, a Queue's item there. */
static int sp_sched_block_timeout(sp_thread **waitlist, double deadline, sp_mutex *mutex, sp_wlock *wl, int (*no_wait)(void *), void *no_wait_arg) {
  sp_thread *self = g_current;
  if (mutex && SP_ATOMIC_LOAD(&mutex->owner, __ATOMIC_SEQ_CST) != self)
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  self->timer_expired = 0;
  wl_lock(wl);
  /* A pending #kill/#raise is checked, and the wait published, with the
     thread's inject slot held, as in sp_sched_block_wl. */
  sp_Fiber *inj = sp_thread_inject_fiber(self);
  int held = 0;
  if (inj) {
    int ik = sp_fiber_inject_hold(inj);
    if (ik) {
      sp_fiber_inject_release(inj, ik);
      wl_unlock(wl);
      sp_fiber_fire_inject_if_pending();   /* raises; does not return (a deferred one is left pending) */
      wl_lock(wl);
    }
    else held = 1;
  }
  if (no_wait && no_wait(no_wait_arg)) {
    if (held) sp_fiber_inject_release(inj, 0);
    wl_unlock(wl);
    return 1;
  }
  NTIMED_INC();   /* before it stops running */
  SP_TH_SET_STATE(self, SP_TH_BLOCKED);
  SP_ATOMIC_STORE(&self->park, SP_PK_PARKING, __ATOMIC_SEQ_CST);   /* on a list, still on its stack */
  self->wait_head = waitlist;
  SP_ATOMIC_STORE(&self->wait_lock, wl, __ATOMIC_RELEASE);
  if (self->repark_front) {
    self->repark_front = 0;
    self->wait_next = *waitlist;
    *waitlist = self;
  }
  else {
    self->wait_next = NULL;
    sp_thread **pp = waitlist;
    while (*pp) pp = &(*pp)->wait_next;
    *pp = self;
  }
  if (held) sp_fiber_inject_release(inj, 0);
  /* scheduled once it is on the list: a timer that fires at once finds it */
  int nudge = 0;
  uint64_t tm = sp_timer_schedule(self, deadline, SP_TIMER_WAIT, &nudge);
  if (!tm) {
    for (sp_thread **pp = waitlist; *pp; pp = &(*pp)->wait_next)
      if (*pp == self) { *pp = self->wait_next; break; }
    self->wait_next = NULL; self->wait_head = NULL;
    SP_ATOMIC_STORE(&self->wait_lock, NULL, __ATOMIC_RELEASE);
    SP_ATOMIC_STORE(&self->park, SP_PK_RUN, __ATOMIC_SEQ_CST);
    SP_TH_SET_STATE(self, SP_TH_RUNNING);
    wl_unlock(wl);
    NTIMED_DEC();
    return -1;
  }
  SP_ATOMIC_STORE(&self->wait_timer, tm, __ATOMIC_RELEASE);
  wl_unlock(wl);
  sp_sysmon_nudge(nudge);
  if (mutex) {   /* released once we are on the list (ConditionVariable#wait) */
    SP_ATOMIC_STORE(&mutex->owner, NULL, __ATOMIC_SEQ_CST);
    sp_mutex_wake_waiter(mutex);
  }
  if (self == &g_main_thread) {
    SCHED_LOCK();
    int running;
    for (;;) {
      sp_sched_pump(NULL, 1);
      running = SP_TH_STATE(self) == SP_TH_RUNNING;
      if (running || sched_main_leave_deadlocked_wait(self)) break;
    }
    SCHED_UNLOCK();
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    NTIMED_DEC();   /* running again */
    if (!running) sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
    /* a #raise that woke main fires here, or once its mutex is held again */
    if (mutex) sp_fiber_defer_inject();
    else sp_fiber_fire_inject_if_pending();
  }
  else {
    void *exc_snap = sp_snap_take();
    sp_exc_ctx_save(exc_snap);
    if (mutex) sp_fiber_defer_inject();
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_snap_give(exc_snap);
    SP_ATOMIC_STORE(&self->wait_timer, 0, __ATOMIC_RELEASE);
    NTIMED_DEC();   /* running again */
    if (!mutex) sp_fiber_fire_inject_if_pending();
  }
  return self->timer_expired ? 0 : 1;
}
#endif

/* Move one thread off `*waitlist` back onto the run queue (or mark the main
   thread runnable so its pump returns). Returns the woken thread, or NULL. */
static sp_thread *sp_sched_wake_one(sp_thread **waitlist) {
  sp_thread *t = *waitlist;
  if (!t) return NULL;
  *waitlist = t->wait_next;
#ifdef SP_THREADS
  sp_timer_cancel(t);
#endif
  t->wait_next = NULL;
  t->wait_head = NULL;
  if (t == &g_main_thread) { SP_TH_SET_STATE(t, SP_TH_RUNNABLE); SCHED_WAKE_MAIN(); return t; }
  sched_make_runnable(t);   /* queued now, or by its worker once it is off its stack */
  return t;
}

/* Remove a parked thread from whatever wait list it sits on (for #kill/#raise). */
/* Answers whether it took t off a list: a waker that got there first, under
   the list's own lock, owns t's wake, and its wait's result with it. */
/* With `only` set (a timer firing), only the wait that timer was published
   with: the thread may have been woken since the timer was taken off the heap,
   and be parked in a newer wait the timer must not end. */
static int sp_sched_unpark_only(sp_thread *t, uint64_t only) {
  /* a list with its own lock is unlinked under it. The thread may be
     woken and re-park elsewhere between the read and the lock, so the lock is
     checked again once held. */
  sp_wlock *wl;
  for (;;) {
    wl = SP_ATOMIC_LOAD(&t->wait_lock, __ATOMIC_ACQUIRE);
    if (!wl) break;
    wl_lock(wl);
    if (SP_ATOMIC_LOAD(&t->wait_lock, __ATOMIC_ACQUIRE) == wl) break;
    wl_unlock(wl);
  }
  if (!t->wait_head || (only && SP_ATOMIC_LOAD(&t->wait_timer, __ATOMIC_ACQUIRE) != only)) {
    if (wl) wl_unlock(wl);
    return 0;
  }
#ifdef SP_THREADS
  sp_timer_cancel(t);
#endif
#ifdef SP_EV_BACKEND
  /* parked on I/O in its home worker's set: wl is that worker's io_wl */
  if (sp_io_parked_on(t, sp_ev_home_of(t))) {
    sp_ev_drop_w(sp_ev_home_of(t), t);
    if (wl) wl_unlock(wl);
    return 1;
  }
#endif
  if (t->wait_head == &g_io_waiters) io_waiters_unlink(t);
  else if (t->wait_head == &t->sleep_slot) { sleep_unlink(t); if (wl) wl_unlock(wl); return 1; }
  else
    for (sp_thread **pp = t->wait_head; *pp; pp = &(*pp)->wait_next)
      if (*pp == t) { *pp = t->wait_next; break; }
  t->wait_next = NULL;
  t->wait_head = NULL;
  SP_ATOMIC_STORE(&t->wait_lock, NULL, __ATOMIC_RELEASE);
  if (wl) wl_unlock(wl);
  return 1;
}
static int sp_sched_unpark(sp_thread *t) { return sp_sched_unpark_only(t, 0); }
/* Main found nothing left that could wake it from a wait (its pump came back
   with main not running): it leaves the wait before it raises the deadlock,
   off the list and running again, so a program that rescues the error and
   uses the object again does not find main still waiting on it -- the next
   push or signal went to that stale entry. Answers 0 when a waker took main
   off the list first: its wake is on the way, and main pumps again.
   PRE: sched lock held (the order is the scheduler lock, then a wait lock). */
static int sched_main_leave_deadlocked_wait(sp_thread *self) {
  if (!sp_sched_unpark(self)) return 0;
  SP_ATOMIC_STORE(&self->park, SP_PK_RUN, __ATOMIC_SEQ_CST);
  SP_TH_SET_STATE(self, SP_TH_RUNNING);
  return 1;
}

#ifdef SP_THREADS
static void sp_sched_timer_expire(const sp_tm_ent *timer, double now) {
  (void)now;
  sp_thread *t = timer->thread;

  /* Off its list first, and only then a timeout. A list under its own lock
     (a ConditionVariable's, a Queue's, a worker's I/O) is woken without the
     scheduler lock, so the wake can land between the look above and this
     unlink: the waker then owns the wake and the wait's result, and marking it
     timed out (or its I/O unready) as well reported a timeout for a wait that
     had been signalled, or for a descriptor that was ready. */
  if (!sp_sched_unpark_only(t, timer->gen)) return;
  if (timer->kind == SP_TIMER_WAIT) t->timer_expired = 1;
  if (timer->kind == SP_TIMER_IO) {
#ifdef SP_EV_BACKEND
    SP_STAT_ADD(g_ev_timeouts, 1);
#endif
    t->io_revents = 0;
    t->io_fd = -1;
  }
  if (t == &g_main_thread) {
    if (SP_TH_STATE(t) == SP_TH_BLOCKED) SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
    SCHED_WAKE_MAIN();
  }
  else sched_make_runnable(t);
}
#endif

/* Queue a raise on the root fiber main runs on and wake a main parked in a
   wait; the scheduler lock is held. The slot holds one, and an exit keeps it:
   a raise taking its place could be rescued, and main would go on as if nothing
   had ended it. */
static void sp_thread_raise_main_locked(const char *cls, const char *msg, void *obj) {
  sp_thread *t = &g_main_thread;
  if (!g_main_root) return;
  sp_fiber_set_raise_inject_behind_exit(g_main_root, cls, msg, obj);
  if (SP_TH_STATE(t) == SP_TH_BLOCKED) {
    sp_sched_unpark(t);
    SP_TH_SET_STATE(t, SP_TH_RUNNABLE);
  }
  SCHED_WAKE_MAIN();   /* a joining main pumps without being BLOCKED */
}

/* #kill / #raise: deliver an inject to the target so it terminates (running its
   ensures) or raises. The current thread acts on itself immediately; another
   thread gets the inject queued on its fiber and is made runnable, so the inject
   fires when the scheduler next runs it -- at body entry (never-run) or right
   after it unblocks (sp_sched_block / Thread.pass). */
static void sp_thread_deliver(sp_thread *t, int is_kill,
                              const char *cls, const char *msg, void *obj) {
  if (t == g_current) {
    if (t == &g_main_thread) return;   /* killing the main thread from itself: unsupported, no-op */
    if (is_kill) sp_fiber_raise_kill_self();   /* noreturn */
    sp_fiber_reraise(cls, msg, obj);           /* noreturn */
  }
  SCHED_LOCK();
  if (SP_TH_STATE(t) == SP_TH_DEAD) { SCHED_UNLOCK(); return; }   /* raced with its death: no-op */
  /* #raise on the main thread from a sibling: queued on the root fiber main
     runs on, and fired where any thread's is, at main's next wait or
     Thread.pass; a main parked in one is woken for it, as #wakeup wakes it.
     It was dropped, and a main waiting on a Queue the raiser was never going
     to fill then deadlocked. #kill of main is still a no-op: CRuby ends the
     process for it (see the Thread row in docs/limitations.md). */
  if (t == &g_main_thread) {
    if (!is_kill) sp_thread_raise_main_locked(cls, msg, obj);
    SCHED_UNLOCK();
    return;
  }
  if (!t->fiber) { SCHED_UNLOCK(); return; }
  /* Published under the inject slot's lock, and the state read after it: a
     thread parking under a lock of its own (an I/O wait) checks for the
     inject and publishes itself BLOCKED with that slot held
     (sp_fiber_inject_hold), so this finds it either not yet checked, and it
     raises, or BLOCKED and on its list. */
  if (is_kill) sp_fiber_set_kill_inject(t->fiber);
  else sp_fiber_set_raise_inject(t->fiber, cls, msg, obj);
#ifdef SP_CEXT
  /* UBF must only unblock native work; it must not call Ruby. Keeping the
     registration locked prevents its data from expiring during delivery. */
  if (t->cext_unblock) t->cext_unblock(t->cext_unblock_data);
#endif
  if (SP_TH_STATE(t) == SP_TH_BLOCKED) {
    sp_sched_unpark(t);
    /* runq_requeue, not rq_push: a STARTED thread is pinned to its home
       worker's TLS (home_wid) and must not resume on another worker. */
    sched_make_runnable(t);
  }
  /* RUNNABLE threads are already queued; the inject fires when they run. */
  SCHED_UNLOCK();
}

#if defined(SP_CEXT) && defined(SP_THREADS)
int sp_cext_native_register(void (*unblock)(void *), void *data,int repeat) {
  SCHED_LOCK();
  sp_thread *t = g_current;
  int pending = t && t->fiber && sp_fiber_inject_pending(t->fiber);
  if (t && !pending) {
    t->cext_unblock = unblock;
    t->cext_unblock_data = data;
    t->cext_repeat_unblock = repeat;
  }
  SCHED_UNLOCK();
  return !pending;
}
void sp_cext_native_unregister(void) {
  SCHED_LOCK();
  if (g_current) {
    g_current->cext_unblock = NULL;
    g_current->cext_unblock_data = NULL;
    g_current->cext_repeat_unblock = 0;
  }
  SCHED_UNLOCK();
}
#endif

/* The DEAD check lives inside sp_thread_deliver, under the sched lock: t may be
   running on another worker and die concurrently, so an unlocked read here
   would race with run_thread_once's state write. */
sp_thread *sp_Thread_kill(sp_thread *t) {
  sp_thread_deliver(t, 1, NULL, NULL, NULL);
  return t;
}
sp_thread *sp_Thread_raise(sp_thread *t, const char *cls, const char *msg, void *obj) {SP_GC_ROOT_STR(msg);
  sp_thread_deliver(t, 0, cls, msg, obj);
  return t;
}

/* ---- Queue ---- */

static void sp_queue_scan(void *p) {
  sp_queue *q = (sp_queue *)p;
  for (sp_int i = 0; i < q->len; i++)
    sp_mark_rbval(q->buf[(q->head + i) % q->cap]);
}
static void sp_queue_fin(void *p) { sp_queue *q = (sp_queue *)p; free(q->buf); }

sp_queue *sp_Queue_new(void) {
  sp_queue *q = (sp_queue *)sp_gc_alloc(sizeof(sp_queue), sp_queue_fin, sp_queue_scan);
  q->cap = 8;
  q->head = q->len = 0;
  q->max = 0;
  q->pop_waiters = NULL;
  q->push_waiters = NULL;
  q->closed = 0;
  q->wl = 0;
  q->buf = (sp_RbVal *)malloc(sizeof(sp_RbVal) * q->cap);
  if (!q->buf) sp_raise_cls("NoMemoryError", "failed to allocate queue");
  return q;
}

sp_queue *sp_SizedQueue_new(sp_int max) {
  if (max <= 0) sp_raise_cls("ArgumentError", "queue size must be positive");
  sp_queue *q = sp_Queue_new();
  q->max = max;
  return q;
}

void sp_Queue_push_options_check(sp_queue *q, int has_non_block, int has_timeout) {
  if (q->max > 0) return;
  /* CRuby's Queue#push takes one argument; a timeout: hash counts as one more. */
  if (has_non_block && has_timeout) sp_raise_cls("ArgumentError", "wrong number of arguments (given 3, expected 1)");
  if (has_non_block || has_timeout) sp_raise_cls("ArgumentError", "wrong number of arguments (given 2, expected 1)");
}

/* (#8702) A Queue's buffer, its closed flag and its two waiter lists are
   guarded by the queue's own wait lock (q->wl), not the scheduler lock, as a
   Mutex's and a ConditionVariable's are: a push that hands a value to a
   parked popper, and the pop that frees a slot for a parked pusher, touch
   nothing global. A waiter parks with sp_sched_block_wl under q->wl after
   looking at the queue under it, and a waker takes it off its list under
   q->wl and wakes it after dropping the lock (sched_wake_unlinked), so a
   wake is not lost and the scheduler lock is taken only where the wake needs
   it (main, a timed waiter's timer). A timed wait keeps the scheduler lock
   for its timer and looks at the queue again under q->wl before it parks. */
static int sp_queue_has_room(void *p) { sp_queue *q = (sp_queue *)p; return q->closed || q->max <= 0 || q->len < q->max; }
static int sp_queue_has_item(void *p) { sp_queue *q = (sp_queue *)p; return q->closed || q->len > 0; }
/* Wake every waiter on `*list`, a list under q->wl (held on entry, dropped on
   return): collected under the lock and woken after it, as
   ConditionVariable#broadcast does, since a woken thread can run and park
   again at once. */
static void sp_queue_wake_all(sp_queue *q, sp_thread **list) {
  for (;;) {
    sp_thread *ts[64]; int n = 0;
    for (sp_thread *w; n < 64 && (w = wl_take_one(list)) != NULL; ) ts[n++] = w;
    wl_unlock(&q->wl);
    for (int i = 0; i < n; i++) sched_wake_unlinked(ts[i]);
    if (n < 64) return;
    wl_lock(&q->wl);
  }
}

static int sp_queue_push_impl(sp_queue *q, sp_RbVal v, int non_block,
                              int timed, double seconds) {
  /* On a full SizedQueue, block until a #pop frees a slot. Root v across the
     block: it lives in this (possibly suspended) frame, and the parking
     thread's saved roots only cover the shadow stack. */
  SP_GC_ROOT_RBVAL(v);
  SP_GC_ROOT(q);
  /* A negative timeout is already expired (CRuby): no ArgumentError. */
  double deadline = timed ? sp_monotonic_now() + seconds : 0.0;
  wl_lock(&q->wl);
  for (;;) {
    if (q->closed) { wl_unlock(&q->wl); sp_raise_cls("ClosedQueueError", "queue closed"); }
    if (q->max <= 0 || q->len < q->max) break;
    if (non_block) { wl_unlock(&q->wl); sp_raise_cls("ThreadError", "queue full"); }
    if (timed && !(seconds > 0.0)) { wl_unlock(&q->wl); return 0; }
    if (timed) {
      wl_unlock(&q->wl);
#ifdef SP_THREADS
      if (!g_sysmon_started) {
        /* Before the first Thread, no green thread can free a slot. There is
           no monitor to fire a timer, so wait directly and check the queue. */
        double remaining = deadline - sp_monotonic_now();
        if (remaining > 0.0) sp_sched_sleep(remaining);
        wl_lock(&q->wl);
        if (q->max > 0 && q->len >= q->max && !q->closed) { wl_unlock(&q->wl); return 0; }
        continue;
      }
      int woken = sp_sched_block_timeout(&q->push_waiters, deadline, NULL, &q->wl, sp_queue_has_room, q);
      if (woken < 0) sp_raise_cls("NoMemoryError", "failed to schedule queue timeout");
      wl_lock(&q->wl);
      if (!woken && !sp_queue_has_room(q)) { wl_unlock(&q->wl); return 0; }
#else
      double remaining = deadline - sp_monotonic_now();
      if (remaining > 0.0) sp_sleep((sp_float)remaining);
      wl_lock(&q->wl);
      if (q->max > 0 && q->len >= q->max && !q->closed) { wl_unlock(&q->wl); return 0; }
#endif
    }
    else sp_sched_block_wl(&q->push_waiters, &q->wl, 0, NULL);   /* q->wl held again on return */
  }
  if (q->len == q->cap) {
    sp_int nc = q->cap * 2;
    sp_RbVal *nb = (sp_RbVal *)malloc(sizeof(sp_RbVal) * nc);
    if (!nb) { wl_unlock(&q->wl); sp_raise_cls("NoMemoryError", "failed to grow queue"); }
    for (sp_int i = 0; i < q->len; i++) nb[i] = q->buf[(q->head + i) % q->cap];
    free(q->buf);
    q->buf = nb; q->cap = nc; q->head = 0;
  }
  /* The barrier goes here, with the store, under the queue's lock. At the
     top of the function, before the lock, two threads pushing onto one old
     queue raced on its header word, one reading the dirty bit while the
     other set it (ThreadSanitizer: sp_gc_wb against sp_gc_wb_slow). It also
     came before a wait for room that can let a collection run, and a
     collection clears the bit: the barrier has to follow the last point
     that can collect, as run_thread_once's second one does. */
  sp_gc_wb((void *)q);
  q->buf[(q->head + q->len) % q->cap] = v;
  q->len++;
  sp_thread *w = wl_take_one(&q->pop_waiters);   /* hand the new value to a waiting popper */
  wl_unlock(&q->wl);
  if (w) sched_wake_unlinked(w);
  return 1;
}

void sp_Queue_push(sp_queue *q, sp_RbVal v) {
  (void)sp_queue_push_impl(q, v, 0, 0, 0.0);
}

void sp_Queue_push_nb(sp_queue *q, sp_RbVal v) {
  (void)sp_queue_push_impl(q, v, 1, 0, 0.0);
}

sp_bool sp_Queue_push_timeout(sp_queue *q, sp_RbVal v, double seconds) {
  return sp_queue_push_impl(q, v, 0, 1, seconds);
}

/* Take the head item; q->wl held on entry, dropped on return. A SizedQueue's
   freed slot goes to a parked pusher. */
static sp_RbVal sp_queue_take(sp_queue *q) {
  sp_RbVal v = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  sp_thread *w = q->max > 0 ? wl_take_one(&q->push_waiters) : NULL;   /* a slot freed up */
  wl_unlock(&q->wl);
  if (w) sched_wake_unlinked(w);
  return v;
}

sp_RbVal sp_Queue_pop(sp_queue *q) {
  /* Block until an element is available. A closed, drained queue returns nil
     rather than blocking forever (CRuby behaviour). */
  SP_GC_ROOT(q);
  wl_lock(&q->wl);
  while (q->len == 0) {
    if (q->closed) { wl_unlock(&q->wl); return sp_box_nil(); }
    sp_sched_block_wl(&q->pop_waiters, &q->wl, 0, NULL);   /* q->wl held again on return */
  }
  return sp_queue_take(q);
}

sp_RbVal sp_Queue_pop_timeout(sp_queue *q, double seconds) {
  SP_GC_ROOT(q);
  /* A negative timeout is already expired (CRuby): no ArgumentError. */
  double deadline = sp_monotonic_now() + seconds;
  wl_lock(&q->wl);
  while (q->len == 0) {
    if (q->closed || !(seconds > 0.0)) { wl_unlock(&q->wl); return sp_box_nil(); }
    wl_unlock(&q->wl);
#ifdef SP_THREADS
    if (!g_sysmon_started) {
      /* Before the first Thread, no green thread can add an item. */
      double remaining = deadline - sp_monotonic_now();
      if (remaining > 0.0) sp_sched_sleep(remaining);
      wl_lock(&q->wl);
      if (q->len == 0) { wl_unlock(&q->wl); return sp_box_nil(); }
      continue;
    }
    int woken = sp_sched_block_timeout(&q->pop_waiters, deadline, NULL, &q->wl, sp_queue_has_item, q);
    if (woken < 0) sp_raise_cls("NoMemoryError", "failed to schedule queue timeout");
    wl_lock(&q->wl);
    if (!woken && !sp_queue_has_item(q)) { wl_unlock(&q->wl); return sp_box_nil(); }
#else
    double remaining = deadline - sp_monotonic_now();
    if (remaining > 0.0) sp_sleep((sp_float)remaining);
    wl_lock(&q->wl);
    if (q->len == 0) { wl_unlock(&q->wl); return sp_box_nil(); }
#endif
  }
  return sp_queue_take(q);
}

sp_RbVal sp_Queue_pop_nb(sp_queue *q) {
  /* Queue#pop(truthy): no_wait. Raise ThreadError on an empty queue, return
     the value otherwise. A closed queue returns nil. */
  wl_lock(&q->wl);
  if (q->len == 0) {
    int closed = q->closed;
    wl_unlock(&q->wl);
    if (closed) return sp_box_nil();
    sp_raise_cls("ThreadError", "queue empty");
  }
  return sp_queue_take(q);
}

sp_int  sp_Queue_size(sp_queue *q)   { wl_lock(&q->wl); sp_int n = q->len;          wl_unlock(&q->wl); return n; }
sp_bool sp_Queue_empty(sp_queue *q)  { wl_lock(&q->wl); sp_bool e = q->len == 0;     wl_unlock(&q->wl); return e; }
sp_int  sp_Queue_max(sp_queue *q)    { wl_lock(&q->wl); sp_int m = q->max;          wl_unlock(&q->wl); return m; }
sp_bool sp_Queue_closed(sp_queue *q) { wl_lock(&q->wl); sp_bool c = q->closed != 0; wl_unlock(&q->wl); return c; }
/* Threads parked in #pop, plus in #push on a full SizedQueue. */
sp_int  sp_Queue_num_waiting(sp_queue *q) {
  wl_lock(&q->wl);
  sp_int n = 0;
  for (sp_thread *t = q->pop_waiters; t; t = t->wait_next) n++;
  for (sp_thread *t = q->push_waiters; t; t = t->wait_next) n++;
  wl_unlock(&q->wl);
  return n;
}
void     sp_Queue_clear(sp_queue *q)  {
  wl_lock(&q->wl);
  q->head = q->len = 0;
  if (q->max > 0) sp_queue_wake_all(q, &q->push_waiters);   /* all slots free; drops q->wl */
  else wl_unlock(&q->wl);
}

void sp_Queue_close(sp_queue *q) {
  wl_lock(&q->wl);
  q->closed = 1;
  /* wake every blocked popper (they return nil) and pusher (they raise) */
  sp_queue_wake_all(q, &q->pop_waiters);   /* drops q->wl */
  wl_lock(&q->wl);
  sp_queue_wake_all(q, &q->push_waiters);
}

/* ---- Mutex ----
 * A non-recursive lock. At N=1 there is no preemption, so a Mutex only matters
 * across a yield: a green thread holding the lock blocks (Queue/CondVar/IO) and
 * another tries to acquire it. unlock hands ownership directly to the next
 * waiter, so the wakeup is a clean transfer (no re-contention). */

sp_mutex *sp_Mutex_new(void) {
  sp_mutex *m = (sp_mutex *)sp_gc_alloc(sizeof(sp_mutex), NULL, NULL);
  m->owner = NULL;
  m->waiters = NULL;
  m->nwaiters = 0;
  return m;
}

sp_mutex *sp_Monitor_new(void) {
  sp_mutex *m = sp_Mutex_new();
  if (m) m->reentrant = 1;
  return m;
}

const char *sp_Mutex_class_name(sp_mutex *m) {
  return (m && m->reentrant) ? "Monitor" : "Thread::Mutex";
}/* A bounded queue is a SizedQueue; an unbounded one is a Queue. One object
   backs both, so the class name reads the bound rather than a separate tag. */
const char *sp_Queue_class_name(sp_queue *q) {
  return (q && q->max > 0) ? "Thread::SizedQueue" : "Thread::Queue";
}


/* An UNCONTENDED lock/unlock does not touch the global scheduler lock.
 *
 * Both entries used to take it unconditionally, so every Ruby-level Mutex
 * operation was a round trip through the one lock that also guards every run
 * queue. A server taking a single lock per request stopped scaling at two OS
 * workers: 12 workers served no more than 2 did, on 1.5 cores, with two of
 * them holding most of the CPU (#4346). At one worker the same lock is free,
 * which is the signature of contention rather than cost.
 *
 * The waiter list still lives under the scheduler lock, since parking and
 * waking are its business. What the fast paths need is agreement about
 * whether anyone is parked, and that is `nwaiters`. The two sides publish in
 * opposite orders -- a waiter counts itself and then re-reads `owner`, an
 * unlocker clears `owner` and then re-reads `nwaiters` -- so with sequential
 * consistency at least one of them observes the other and no wake is lost. */
/* A contended lock spins briefly before it parks. The slow path below is a
   scheduler-lock round trip to park plus another to be woken, and the wake
   itself is a scheduling latency (the [mutex-wait] histogram put the median
   contended acquisition at 64-128 us). A critical section guarding a hash
   read holds the mutex for a microsecond, so an owner that is RUNNING on
   another worker will let go long before a park would complete; wait for it
   in place, for a bounded number of pauses. The spin stops as soon as the
   owner is not on a CPU: an owner parked on I/O or preempted inside the
   section will not release it soon, and the only thread running on THIS
   worker is us, so a running owner is by construction elsewhere. campfire's
   messages page at 64 connections parked 58k times a second on its
   fragment-cache shards (3,200 req/s); spinning first restores most of the
   throughput the parks took. */
/* SP_CPU_RELAX: the architecture's pause instruction, sp_compat.h */
#ifndef SP_MUTEX_SPIN
#define SP_MUTEX_SPIN 256
#endif
static inline int sp_mutex_spin_acquire(sp_mutex *m, sp_thread *self) {
  for (int i = 0; i < SP_MUTEX_SPIN; i++) {
    sp_thread *o = SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST);
    if (o == NULL) {
      sp_thread *expect = NULL;
      if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
      continue;
    }
    if (o == self) return 0;   /* reentrancy or a deadlock: the slow path decides */
    if (SP_ATOMIC_LOAD(&o->state, __ATOMIC_SEQ_CST) != SP_TH_RUNNING) return 0;
    SP_CPU_RELAX();
  }
  return 0;
}

void sp_Mutex_lock(sp_mutex *m) {
  sp_thread *self = g_current;
  sp_thread *expect = NULL;
  if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    { if (sched_lat_enabled()) SP_ATOMIC_FETCH_ADD(&g_mtx_fast, 1, __ATOMIC_RELAXED); return; }   /* was unlocked: ours, no lock taken */
  if (sp_mutex_spin_acquire(m, self))
    { if (sched_lat_enabled()) SP_ATOMIC_FETCH_ADD(&g_mtx_spin, 1, __ATOMIC_RELAXED); return; }
  wl_lock(&m->wl);
  /* the owner re-entering its own Monitor just goes deeper */
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self && m->reentrant) { m->depth++; wl_unlock(&m->wl); return; }
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self) { wl_unlock(&m->wl); sp_raise_cls("ThreadError", "deadlock; recursive locking"); }
  expect = NULL;
  if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    { wl_unlock(&m->wl); return; }
  /* Count ourselves BEFORE the last look at `owner`: an unlocker that clears
     it after this point re-reads the count and finds us. The count changes
     only under the scheduler lock, but the unlock fast path reads it without
     the lock, so every change is atomic: a plain one is a data race against
     that read (ThreadSanitizer: sp_Mutex_lock against sp_Mutex_unlock). */
  SP_ATOMIC_FETCH_ADD(&m->nwaiters, 1, __ATOMIC_SEQ_CST);
  SP_ATOMIC_FENCE(__ATOMIC_SEQ_CST);
  expect = NULL;
  if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    SP_ATOMIC_FETCH_SUB(&m->nwaiters, 1, __ATOMIC_SEQ_CST);   /* released under us; took it instead */
    wl_unlock(&m->wl);
    return;
  }
  /* The unlocker clears `owner` and wakes us; we take the mutex ourselves
     with the same exchange the fast path uses, and park again if a running
     thread took it first. No hand-off: handing a contended mutex to a PARKED
     waiter made the critical section's throughput the scheduling latency of
     a wake (milliseconds on a busy worker), and every thread behind it paid
     that per acquisition -- a lock convoy, 16-64 ms waits on campfire's
     fragment-cache shards at 64 connections. A waiter that loses the race
     goes back to the FRONT of the list, so the order of arrival still
     decides who is offered the mutex next. */
  double mt0 = sched_lat_enabled() ? sp_monotonic_now() : 0;
  for (;;) {
    sp_sched_block_wl(&m->waiters, &m->wl, 0, NULL);
    expect = NULL;
    if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    self->repark_front = 1;   /* re-park at the head (sp_sched_block honours this) */
  }
  if (mt0 > 0) sched_hist_add(g_mtx_hist, &g_mtx_n, &g_mtx_max, (sp_monotonic_now() - mt0) * 1e6);
  SP_ATOMIC_FETCH_SUB(&m->nwaiters, 1, __ATOMIC_SEQ_CST);
  wl_unlock(&m->wl);
}

void sp_Mutex_unlock(sp_mutex *m) {
  sp_thread *self = g_current;
  /* depth is the owner's own field, so reading it as the owner needs no lock;
     a non-owner fails the exchange below and takes the slow path, which is
     where the ThreadError is raised. */
  if (m->depth == 0 && SP_ATOMIC_LOAD(&m->nwaiters, __ATOMIC_SEQ_CST) == 0) {
    sp_thread *expect = self;
    if (SP_ATOMIC_CAS(&m->owner, &expect, NULL, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      if (SP_ATOMIC_LOAD(&m->nwaiters, __ATOMIC_SEQ_CST) == 0) return;
      /* A waiter counted itself while we were releasing. It is parked, or is
         about to look at `owner` one more time and take it; either way the
         list is the authority, so finish the hand-off under the lock.

         The hand-off has to be a compare-exchange, not a load and a store.
         The lock's own fast path takes a free mutex WITHOUT the scheduler
         lock, so a third thread can claim it between the load that finds it
         free and the store that gives it to the waiter -- and the store then
         puts the waiter's name over that owner, leaving TWO threads inside
         the critical section. Which of them notices is whichever unlocks
         second: its fast-path exchange fails, and it raises "Attempt to
         unlock a mutex which is not locked" against a mutex it really did
         hold. Reproduced with eight workers on a hot lock, roughly one run in
         five.

         The waiter is peeked rather than woken first, so a failed exchange
         leaves it on the list where it was. Nothing is lost by declining: a
         listed waiter is a counted one, so the thread that won the race sees
         nwaiters > 0 and cannot take this fast path when it unlocks. */
      sp_mutex_wake_waiter(m);   /* it takes the mutex itself, or re-parks */
      return;
    }
  }
  wl_lock(&m->wl);
  if (m->depth > 0 && SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self) { m->depth--; wl_unlock(&m->wl); return; }
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != self) {
    wl_unlock(&m->wl);
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  { sp_thread *w = wl_take_one(&m->waiters); wl_unlock(&m->wl); if (w) sched_wake_unlinked(w); }
}

/* Every read and write of `owner` outside the scheduler lock is atomic, since
   the uncontended lock/unlock paths above no longer take it. */
sp_bool sp_Mutex_try_lock(sp_mutex *m) {
  SCHED_LOCK();
  sp_bool r;
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == g_current && m->reentrant) { m->depth++; r = 1; }
  else {
    sp_thread *expect = NULL;
    r = SP_ATOMIC_CAS(&m->owner, &expect, g_current, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
  }
  SCHED_UNLOCK();
  return r;
}
sp_bool sp_Mutex_locked(sp_mutex *m) { return SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != NULL; }
sp_bool sp_Mutex_owned(sp_mutex *m)  { return SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == g_current; }

/* ---- ConditionVariable ----
 * #wait releases the mutex, parks on the CV, and re-acquires the mutex on
 * wake; #signal wakes one waiter, #broadcast wakes all. */

sp_condvar *sp_CondVar_new(void) {
  sp_condvar *cv = (sp_condvar *)sp_gc_alloc(sizeof(sp_condvar), NULL, NULL);
  cv->waiters = NULL;
  return cv;
}

static sp_RbVal sp_CondVar_wait_impl(sp_condvar *cv, sp_mutex *m) {
  time_t beg = time(NULL);
  /* the owner check and a pending #kill/#raise need no lock; the park
     itself is ordered under the condvar's own (sp_sched_block_wl) */
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current)
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  if (sp_thread_inject_pending(g_current))
    sp_fiber_fire_inject_if_pending();
  double ct0 = sched_lat_enabled() ? sp_monotonic_now() : 0;
  /* on the condvar's list under its own lock, then the mutex released,
     then parked (sp_sched_block_wl) */
  wl_lock(&cv->wl);
  sp_sched_block_wl(&cv->waiters, &cv->wl, 1, m);
  wl_unlock(&cv->wl);
  if (ct0 > 0) sched_hist_add(g_cv_hist, &g_cv_n, &g_cv_max, (sp_monotonic_now() - ct0) * 1e6);
  sp_Mutex_lock(m);   /* re-acquire (may block again on the mutex) */
  sp_fiber_undefer_inject();
  sp_fiber_fire_inject_if_pending();
  return sp_box_int((sp_int)(time(NULL) - beg));
}

/* The single-threaded runtime (or a threaded program before its monitor
   starts) has no timer heap to park on. */
static void sp_CondVar_wait_timeout_blocking(sp_mutex *m, double seconds) {
  SCHED_LOCK();
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  sp_mutex_wake_waiter(m);   /* under m's own wait lock */
  SCHED_UNLOCK();
  while (seconds > 86400.0) {
    struct timespec day = {86400, 0};
    while (nanosleep(&day, &day) == -1 && errno == EINTR) {}
    seconds -= 86400.0;
  }
  struct timespec req = {(time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9)};
  if (req.tv_nsec >= 1000000000L) { req.tv_sec++; req.tv_nsec -= 1000000000L; }
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
  sp_Mutex_lock(m);
}

static sp_RbVal sp_CondVar_wait_nb_impl(sp_condvar *cv, sp_mutex *m);
static sp_RbVal sp_CondVar_wait_timeout_impl(sp_condvar *cv, sp_mutex *m, double seconds) {
  /* CRuby checks the interval before it lets go of the mutex */
  if (seconds < 0.0) sp_raise_cls("ArgumentError", "time interval must not be negative");
  if (!(seconds > 0.0)) return sp_CondVar_wait_nb_impl(cv, m);
#ifdef SP_THREADS
  /* With no spawned green threads there is no monitor yet. Release the mutex
     and sleep directly; the main thread is the only possible signaler in this
     state, so there cannot be a signal to lose, and the wait always times out. */
  if (!g_sysmon_started) {
    sp_CondVar_wait_timeout_blocking(m, seconds);
    return sp_box_nil();
  }
  time_t beg = time(NULL);
  double ct0 = sched_lat_enabled() ? sp_monotonic_now() : 0;
  int woken = sp_sched_block_timeout(&cv->waiters, sp_monotonic_now() + seconds, m, &cv->wl, NULL, NULL);
  if (woken < 0) sp_raise_cls("NoMemoryError", "failed to schedule condition variable timeout");
  if (ct0 > 0) { SCHED_LOCK(); sched_hist_add(g_cv_hist, &g_cv_n, &g_cv_max, (sp_monotonic_now() - ct0) * 1e6); SCHED_UNLOCK(); }
  sp_Mutex_lock(m);
  sp_fiber_undefer_inject();
  sp_fiber_fire_inject_if_pending();
  return woken ? sp_box_int((sp_int)(time(NULL) - beg)) : sp_box_nil();
#else
  sp_CondVar_wait_timeout_blocking(m, seconds);
  return sp_box_nil();
#endif
}

/* CRuby's `wait(mutex, 0)`: release the mutex and return without parking.
   The cv->waiters list is threads blocked on this CV, not pending signals, so
   this path does not touch it. */
static sp_RbVal sp_CondVar_wait_nb_impl(sp_condvar *cv, sp_mutex *m) {
  (void)cv;
  SCHED_LOCK();
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  if (sp_thread_inject_pending(g_current)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
  sp_fiber_defer_inject();
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  sp_mutex_wake_waiter(m);   /* under m's own wait lock */
  SCHED_UNLOCK();
  sp_Mutex_lock(m);
  sp_fiber_undefer_inject();
  sp_fiber_fire_inject_if_pending();
  return sp_box_nil();
}

/* A Monitor (a reentrant Mutex) held more than once is released whole by a
   wait and taken back at the same depth, as CRuby's Monitor#wait_for_cond
   does: left as it was, the depth outlived the release, and a waiter in
   nested synchronize blocks came back holding the count of the thread that
   signalled it (MonitorMixin::ConditionVariable). */
static int sp_cv_depth_take(sp_mutex *m) {
  if (!m->reentrant || SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current) return 0;
  int d = m->depth;
  m->depth = 0;
  return d;
}
sp_RbVal sp_CondVar_wait(sp_condvar *cv, sp_mutex *m) {
  int d = sp_cv_depth_take(m);
  sp_RbVal r = sp_CondVar_wait_impl(cv, m);
  m->depth = d;
  return r;
}
sp_RbVal sp_CondVar_wait_timeout(sp_condvar *cv, sp_mutex *m, double seconds) {
  int d = sp_cv_depth_take(m);
  sp_RbVal r = sp_CondVar_wait_timeout_impl(cv, m, seconds);
  m->depth = d;
  return r;
}
sp_RbVal sp_CondVar_wait_nb(sp_condvar *cv, sp_mutex *m) {
  int d = sp_cv_depth_take(m);
  sp_RbVal r = sp_CondVar_wait_nb_impl(cv, m);
  m->depth = d;
  return r;
}

void sp_CondVar_signal(sp_condvar *cv) {
  wl_lock(&cv->wl);
  sp_thread *t = wl_take_one(&cv->waiters);
  wl_unlock(&cv->wl);
  if (t) sched_wake_unlinked(t);
}
void sp_CondVar_broadcast(sp_condvar *cv) {
  /* collected under the lock and woken after it: a woken thread can run and
     park again at once, rewriting its links, so the list is not walked late */
  sp_thread *few[64], **ts = few; int n = 0, cap = 64;
  wl_lock(&cv->wl);
  for (sp_thread *t = cv->waiters; t; t = t->wait_next) n++;
  if (n > cap) { ts = (sp_thread **)malloc(sizeof(sp_thread *) * (size_t)n); cap = n; }
  n = 0;
  for (sp_thread *t; ts && (t = wl_take_one(&cv->waiters)) != NULL; ) ts[n++] = t;
  wl_unlock(&cv->wl);
  for (int i = 0; i < n; i++) sched_wake_unlinked(ts[i]);
  if (ts != few) free(ts);
}

/* Thread#inspect / #to_s: CRuby's "#<Thread:0xADDR <status>>" shape (the
   source-location segment CRuby inserts is not carried) (#2977). */
const char *sp_Thread_inspect(sp_thread *t) {
  extern const char *sp_sprintf(const char *fmt, ...);
  /* NULL is this type's nil, and nil inspects as "nil". Without this a
     `Thread#join(limit)` that TIMED OUT -- which answers NULL, correctly --
     printed `#<Thread:0x0000000000000000 dead>`, so the one thing the return
     value is there to tell you read as the opposite of what it said (#4394).
     The status word compounded it: the thread it named was still running. */
  if (!t) return "nil";
  const char *st = "dead";
  if (t) {
    switch (SP_TH_STATE(t)) {
      case SP_TH_RUNNING: case SP_TH_RUNNABLE: st = "run"; break;
      case SP_TH_BLOCKED: st = "sleep"; break;
      default: st = t->has_exc ? "aborting" : "dead"; break;
    }
  }
  /* CRuby carries the creation site between the address and the status */
  if (t && t->birth_file)
    return sp_sprintf("#<Thread:0x%016llx %s:%lld %s>",
                      (unsigned long long)(uintptr_t)t, t->birth_file,
                      (long long)t->birth_line, st);
  return sp_sprintf("#<Thread:0x%016llx %s>", (unsigned long long)(uintptr_t)t, st);
}

#if defined(SP_THREADS) && !defined(_WIN32) && !defined(__wasi__)
/* fork(2) once threads have started (#10516). The child has the calling OS
   thread alone: the monitor, the helper workers, the collector's sweepers and
   the trimmer stay behind, a lock one of them held stays held, and the flags
   that say they run say so still -- a sleep then waited for a monitor that
   was not there. CRuby's child keeps the thread that forked and no other;
   here that is the main thread, which the child goes on running on worker 0.

   The parent stops the world as a collection does, so no worker is inside
   the runtime's locks or the allocator, and waits out a concurrent sweep;
   the child then starts over from one worker: its locks fresh, every other
   green thread dead and off every queue, list and timer, and the pool, the
   monitor, the sweepers and their hooks unstarted, to come up again on the
   child's first Thread or wait as they did at boot. */
static void sp_sched_fork_child(void) {
  pthread_mutex_init(&g_sched_lock, NULL);
  pthread_mutex_init(&g_reg_lock, NULL);
  for (int w = 0; w < SP_MAX_WORKERS; w++) pthread_mutex_init(&g_rq_lock[w].m, NULL);
  pthread_mutex_init(&g_cs_lock, NULL);
  /* a condvar waited on in the parent (the sweepers' g_cs_go, the monitor's
     g_stw_release) still counts those waiters in the child, and a broadcast
     there waits for them for good */
  pthread_cond_t *cvs[] = { &g_sweep_cv, &g_mk_cv, &g_sched_work, &g_stw_request, &g_stw_release, &g_sysmon_cv,
                            &g_cs_go, &g_cs_done };
  for (size_t i = 0; i < sizeof cvs / sizeof cvs[0]; i++) pthread_cond_init(cvs[i], NULL);
  g_sched_held = 0;
  g_cnt_other_wl = 0;
  /* the other green threads: dead, and off whatever they waited on */
  sp_thread *self = g_current;
  for (sp_thread *t = g_all, *nx; t; t = nx) {
    nx = t->all_next;
    if (t == self) continue;
    if (t->wait_head) {
      sp_thread **pp = t->wait_head;
      while (*pp && *pp != t) pp = &(*pp)->wait_next;
      if (*pp) *pp = t->wait_next;
    }
    if (t->wait_lock) *t->wait_lock = 0;
    t->wait_next = NULL; t->wait_head = NULL; t->wait_lock = NULL;
    t->joiners = NULL; t->join_wl = 0; t->sleep_wl = 0; t->sleep_slot = NULL;
    t->rq_next = NULL; t->tm_armed = 0; t->wait_timer = 0;
    t->io_fd = -1; t->io_set = NULL; t->io_nset = 0;
    t->retval = sp_box_nil(); t->has_exc = 0;
    SP_ATOMIC_STORE(&t->park, SP_PK_RUN, __ATOMIC_RELAXED);
    SP_TH_SET_STATE(t, SP_TH_DEAD);
    t->all_prev = t->all_next = NULL;
  }
  g_all = NULL;   /* (the main thread is no entry of it) */
  /* the run queues, the timers, the I/O waiters and the counts: all theirs */
  memset(&g_grq, 0, sizeof g_grq);
  memset(g_lrq, 0, sizeof g_lrq);
  g_timer_heap_len = 0;
  for (int w = 0; w < SP_MAX_WORKERS; w++) {
    sp_tm_stage *st = &g_tm_stage[w];
    st->wl = 0; st->len = 0; st->ncancel = 0;
  }
  g_io_waiters = NULL;
  memset(g_cnt, 0, sizeof g_cnt);
  g_cnt_hi = 1;
  g_cnt_me = &g_cnt[0];
  /* worker 0 alone; the readiness sets are the parent's (an epoll set is
     shared across a fork), so each is closed and made again on a first park */
  for (int w = 0; w < SP_MAX_WORKERS; w++) {
    sp_wslot *ws = &g_wslot[w];
    if (ws->evfd > 0) { close(ws->evfd); close(ws->kick[0]); close(ws->kick[1]); }
    if (ws->evfd != -2) ws->evfd = 0;
    ws->kick_armed = 0; ws->ev_waiting = 0;
    pthread_cond_init(&ws->cv, NULL);
    ws->idle = 0; ws->parked = 0; ws->wake_sent = 0; ws->pinned = 0;
    ws->io_wl = 0; ws->io_waiters = NULL;
    if (ws->ev_tab) memset(ws->ev_tab, 0, sizeof *ws->ev_tab * (size_t)ws->ev_cap);
    ws->cur = NULL; ws->since_bits = 0;
    if (w > 0) ws->active = 0;
  }
  g_wslot[0].tid = pthread_self();
  g_wslot[0].cur = self;
#ifdef SP_EV_BACKEND
  g_ev_fd = -1;
#endif
  /* the pool, the monitor, the collector's helpers: none run in the child */
  sp_active_workers = 1;
  g_workers_started = 0; g_helpers_spawned = 0; g_shutdown = 0;
  g_sysmon_started = 0; g_sysmon_idle = 0;
  if (g_sysmon_pipe[0] >= 0) close(g_sysmon_pipe[0]);
  if (g_sysmon_pipe[1] >= 0) close(g_sysmon_pipe[1]);
  g_sysmon_pipe[0] = g_sysmon_pipe[1] = -1;
  g_sysmon_wake_at_bits = 0; g_nearest_bits = 0; g_npreempt = 0;
  g_stw_active = 0; g_nparked = 0; g_n_parked_fiber = 0; g_stw_epoch++;
  g_nnative = 0;
  memset(g_native_out, 0, sizeof g_native_out);
  memset(g_native_nfiber, 0, sizeof g_native_nfiber);
  g_main_waits = 0;
  g_sweep_go = 0; g_sw_ntasks = 0; g_sw_next = 0; g_sw_done = 0;
  g_mk_go = 0; g_mk_joined = 0; g_mk_active = 0;
  g_cs_help = 0; g_cs_ntasks = 0; g_cs_finished = 0; g_cs_running = 0; g_cs_unclaimed = 0;
  g_cs_nsweepers = 0;
  sp_gc_par_sweep_hook = NULL; sp_gc_par_mark_hook = NULL;
  sp_gc_conc_sweep_hook = NULL; sp_gc_conc_wait_hook = NULL;
  sp_gc_trimmer_on = 0;
  sp_recompute_safepoint_flag();
}
#endif

int sp_sched_fork(void) {
#if defined(_WIN32) || defined(__wasi__)
  sp_raise_cls("NotImplementedError", "fork() function is unimplemented on this machine");
  return -1;
#elif defined(SP_THREADS)
  if (!g_workers_started && !g_sysmon_started) return (int)fork();
  /* the main thread goes on in the child; another thread has no worker 0
     under it there to run on */
  if (g_current != &g_main_thread || sp_worker_id != 0)
    sp_raise_cls("NotImplementedError", "fork from a thread other than the main thread");
#ifdef SP_CEXT
  sp_cext_lock();
#endif
  /* the world stopped, as for a collection: every other worker parked at a
     safepoint, none inside a lock of the runtime's */
  SCHED_LOCK();
  while (g_stw_active) sp_stw_park_locked();
  SP_ATOMIC_STORE(&g_stw_active, 1, __ATOMIC_SEQ_CST);
  g_stw_epoch++;
  g_nparked = 0;
  SP_SAFEPOINT_SET(1);
  sched_wake_all_workers(1);
  while (g_nparked + SP_ATOMIC_LOAD(&g_nnative, __ATOMIC_SEQ_CST) < sp_active_workers - 1) pthread_cond_wait(&g_stw_request, &g_sched_lock);
  SCHED_UNLOCK();
  g_collector_active = 1;
  if (sp_gc_conc_wait_hook) sp_gc_conc_wait_hook();   /* the sweepers out of the heap's lists */
  g_collector_active = 0;
  pid_t pid = fork();
  if (pid == 0) {
    sp_sched_fork_child();
#ifdef SP_CEXT
    sp_cext_unlock();
#endif
    return 0;
  }
  SCHED_LOCK();
  g_n_parked_fiber = 0;
  SP_ATOMIC_STORE(&g_stw_active, 0, __ATOMIC_SEQ_CST);
  sp_recompute_safepoint_flag();
  pthread_cond_broadcast(&g_stw_release);
  SCHED_UNLOCK();
#ifdef SP_CEXT
  sp_cext_unlock();
#endif
  return (int)pid;
#else
  return (int)fork();
#endif
}

/* Whether the first Thread has brought up the worker pool, its monitor and the
   collector's sweepers: OS threads a fork does not carry into the child
   (Process.daemon, lib/sp_process.c). Before that the program is one thread. */
int sp_sched_threads_started(void) {
#ifdef SP_THREADS
  return g_workers_started;
#else
  return 0;
#endif
}
