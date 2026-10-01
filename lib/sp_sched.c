/* sp_sched.c -- cooperative M:N thread scheduler bodies, Phase 0 (N=1).
 * See sp_sched.h. Built on the sp_fiber context switch: the main thread runs on
 * the root fiber and pumps a run queue of green threads whenever it blocks. */
#include "sp_sched.h"
#include "sp_alloc.h"   /* sp_box_nil / sp_box_obj */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>     /* sysconf (worker count) */
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
 * One mutex guards all scheduler/sync metadata: the run queue, the live-thread
 * registry, every wait list (joiners, Queue/Mutex/ConditionVariable waiters) and
 * the Queue ring buffers. A worker holds it only while touching that metadata
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
#define SCHED_LOCK()    pthread_mutex_lock(&g_sched_lock)
#define SCHED_UNLOCK()  pthread_mutex_unlock(&g_sched_lock)

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
static int            g_worker_cap = 0;       /* max helper workers = min(cores, SPINEL_WORKERS) */
static int            g_helpers_spawned = 0;  /* helpers created so far; ids 1..g_helpers_spawned */
static pthread_cond_t g_sched_work = PTHREAD_COND_INITIALIZER;   /* idle workers wait for runnable work */
static pthread_cond_t g_stw_request = PTHREAD_COND_INITIALIZER;  /* collector waits for parks */
static pthread_cond_t g_stw_release = PTHREAD_COND_INITIALIZER;  /* parked workers wait for clear */
static pthread_cond_t g_sysmon_cv = PTHREAD_COND_INITIALIZER;    /* the monitor thread waits here */
static pthread_t      g_sysmon;                                  /* monitor: wakes sleepers + preempts */
static int            g_sysmon_started = 0;
static int            g_sysmon_idle = 0; /* monitor is parked on g_sysmon_cv (signal it to start ticking) */
static int            g_sysmon_pipe[2] = { -1, -1 };  /* self-pipe: wake the monitor out of poll() */
static void sp_sched_report_stats(void);   /* SPINEL_SCHED_STATS, below */
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
  if (g_sysmon_idle) pthread_cond_signal(&g_sysmon_cv);
  else if (g_sysmon_pipe[1] >= 0) { char c = 1; ssize_t r = write(g_sysmon_pipe[1], &c, 1); (void)r; }
}

/* The monitor's cached next wake time. Timer insertions lower it; canceling its
   current minimum clears it so the monitor recomputes from the heap. With the
   event backend, the monitor can skip rebuilding the I/O poll set until a
   deadline is due. */
static double g_nearest = 0.0;   /* 0 = nothing timed */
static void sp_deadline_added(double d) {   /* PRE: sched lock held */
  if (d <= 0.0) return;
  if (g_nearest == 0.0 || d < g_nearest) {
    g_nearest = d;
    sp_sysmon_wake();   /* it must shorten its wait */
  }
}

/* Timers are appended to a staging buffer while green threads park, then
   inserted into this min-heap together by the monitor. Most waits run to their
   deadline, so this keeps the park path cheap and amortizes heap repair across
   the batch, like IO::Event::Timers. Canceled handles are left in place until
   they reach the root or compaction makes rebuilding cheaper. */
enum { SP_TIMER_SLEEP, SP_TIMER_IO, SP_TIMER_WAIT };
struct sp_sched_timer {
  double deadline;
  sp_thread *thread;
  int kind;
  int active;
  int in_heap;
  size_t index;
};
#define SP_TIMER_COMPACT_MINIMUM 128
#define SP_TIMER_HEAPIFY_INSERT_RATIO 2
static sp_sched_timer **g_timer_heap = NULL;
static size_t g_timer_heap_len = 0, g_timer_heap_cap = 0;
static sp_sched_timer **g_timer_scheduled = NULL;
static size_t g_timer_scheduled_len = 0, g_timer_scheduled_cap = 0;
static size_t g_timer_cancelled = 0, g_timer_pending = 0;
static void sp_sched_timer_expire(sp_sched_timer *timer, double now);

static int sp_timer_reserve(sp_sched_timer ***items, size_t *capacity, size_t needed) {
  if (*capacity >= needed) return 1;
  size_t nc = *capacity ? *capacity : 16;
  while (nc < needed) {
    if (nc > SIZE_MAX / 2) { nc = needed; break; }
    nc *= 2;
  }
  if (nc > SIZE_MAX / sizeof(sp_sched_timer *)) return 0;
  sp_sched_timer **next = (sp_sched_timer **)realloc(*items, nc * sizeof(sp_sched_timer *));
  if (!next) return 0;
  *items = next;
  *capacity = nc;
  return 1;
}

static void sp_timer_swap(size_t a, size_t b) {
  sp_sched_timer *tmp = g_timer_heap[a];
  g_timer_heap[a] = g_timer_heap[b];
  g_timer_heap[b] = tmp;
  g_timer_heap[a]->index = a;
  g_timer_heap[b]->index = b;
}

static void sp_timer_bubble_up(size_t i) {
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (g_timer_heap[parent]->deadline <= g_timer_heap[i]->deadline) break;
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
                   g_timer_heap[right]->deadline < g_timer_heap[left]->deadline ? right : left;
    if (g_timer_heap[i]->deadline <= g_timer_heap[child]->deadline) return;
    sp_timer_swap(i, child);
    i = child;
  }
}

static void sp_timer_heapify(void) {
  for (size_t i = 0; i < g_timer_heap_len; i++) {
    g_timer_heap[i]->in_heap = 1;
    g_timer_heap[i]->index = i;
  }
  if (g_timer_heap_len > 1) {
    for (size_t i = g_timer_heap_len / 2; i > 0; i--) sp_timer_bubble_down(i - 1);
  }
}

static sp_sched_timer *sp_timer_pop_root(void) {
  if (!g_timer_heap_len) return NULL;
  sp_sched_timer *root = g_timer_heap[0];
  sp_sched_timer *last = g_timer_heap[--g_timer_heap_len];
  if (g_timer_heap_len) {
    g_timer_heap[0] = last;
    last->index = 0;
    sp_timer_bubble_down(0);
  }
  root->in_heap = 0;
  return root;
}

static sp_sched_timer *sp_timer_schedule(sp_thread *thread, double deadline, int kind) {
  sp_sched_timer *timer = (sp_sched_timer *)malloc(sizeof(sp_sched_timer));
  if (!timer) return NULL;
  if (!sp_timer_reserve(&g_timer_scheduled, &g_timer_scheduled_cap, g_timer_scheduled_len + 1) ||
      !sp_timer_reserve(&g_timer_heap, &g_timer_heap_cap,
                        g_timer_heap_len + g_timer_scheduled_len + 1)) {
    free(timer);
    return NULL;
  }
  timer->deadline = deadline;
  timer->thread = thread;
  timer->kind = kind;
  timer->active = 1;
  timer->in_heap = 0;
  timer->index = 0;
  g_timer_scheduled[g_timer_scheduled_len++] = timer;
  thread->timer = timer;
  g_timer_pending++;
  sp_deadline_added(deadline);
  return timer;
}

static void sp_timer_cancel(sp_thread *thread) {
  sp_sched_timer *timer = thread->timer;
  if (!timer) return;
  thread->timer = NULL;
  if (timer->active) {
    timer->active = 0;
    if (g_timer_pending) g_timer_pending--;
    if (timer->in_heap) g_timer_cancelled++;
    if (timer->deadline == g_nearest) {
      g_nearest = 0.0;
      sp_sysmon_wake();
    }
  }
}

static void sp_timer_flush(void) {
  if (!g_timer_scheduled_len &&
      !(g_timer_cancelled >= SP_TIMER_COMPACT_MINIMUM && g_timer_cancelled * 2 > g_timer_heap_len)) return;

  if (g_timer_cancelled >= SP_TIMER_COMPACT_MINIMUM && g_timer_cancelled * 2 > g_timer_heap_len) {
    size_t write = 0;
    for (size_t i = 0; i < g_timer_heap_len; i++) {
      sp_sched_timer *timer = g_timer_heap[i];
      if (timer->active) g_timer_heap[write++] = timer;
      else free(timer);
    }
    g_timer_heap_len = write;
    g_timer_cancelled = 0;
    for (size_t i = 0; i < g_timer_scheduled_len; i++) {
      sp_sched_timer *timer = g_timer_scheduled[i];
      if (timer->active) g_timer_heap[g_timer_heap_len++] = timer;
      else free(timer);
    }
    g_timer_scheduled_len = 0;
    sp_timer_heapify();
    return;
  }

  size_t live = 0;
  for (size_t i = 0; i < g_timer_scheduled_len; i++)
    if (g_timer_scheduled[i]->active) live++;
  int rebuild = g_timer_heap_len == 0 || live > g_timer_heap_len * SP_TIMER_HEAPIFY_INSERT_RATIO;
  for (size_t i = 0; i < g_timer_scheduled_len; i++) {
    sp_sched_timer *timer = g_timer_scheduled[i];
    if (!timer->active) { free(timer); continue; }
    size_t index = g_timer_heap_len++;
    g_timer_heap[index] = timer;
    timer->in_heap = 1;
    timer->index = index;
    if (!rebuild) sp_timer_bubble_up(index);
  }
  g_timer_scheduled_len = 0;
  if (rebuild) sp_timer_heapify();
}

static sp_sched_timer *sp_timer_peek(void) {
  sp_timer_flush();
  while (g_timer_heap_len && !g_timer_heap[0]->active) {
    sp_sched_timer *timer = sp_timer_pop_root();
    if (g_timer_cancelled) g_timer_cancelled--;
    free(timer);
  }
  return g_timer_heap_len ? g_timer_heap[0] : NULL;
}

static double sp_timer_next_deadline(void) {
  sp_sched_timer *timer = sp_timer_peek();
  return timer ? timer->deadline : 0.0;
}

static void sp_timer_fire_due(double now) {
  sp_sched_timer *timer;
  while ((timer = sp_timer_peek()) && timer->deadline <= now) {
    sp_timer_pop_root();
    sp_thread *thread = timer->thread;
    timer->active = 0;
    if (thread->timer == timer) thread->timer = NULL;
    if (g_timer_pending) g_timer_pending--;
    sp_sched_timer_expire(timer, now);
    free(timer);
  }
}

/* ---- preemption (design §5): timeslice tracking + the one safepoint flag ----
 * The monitor watches how long each worker has run its current green thread; past
 * a quantum it sets that thread's preempt_request, raises the safepoint flag, and
 * SIGURGs the worker. The thread yields at its next safepoint poll -- which the
 * codegen emits at loop back-edges, i.e. points that hold no runtime lock, so a
 * preempting thread always parks at a safe point. g_npreempt counts outstanding
 * requests so the flag (shared with GC stop-the-world) is cleared only when no
 * reason remains. All of this is touched solely under g_sched_lock. */
#define SP_PREEMPT_QUANTUM 0.010   /* a green thread runs ~10ms before it must yield */
#define SP_PREEMPT_TICK    0.005   /* monitor re-checks worker timeslices this often while busy */
static int g_npreempt = 0;         /* preempt_requests set but not yet consumed */
static int g_preempt_sig = SIGURG; /* the signal the monitor sends; SPINEL_PREEMPT_SIGNAL overrides */
typedef struct { pthread_t tid; sp_thread *cur; double since; int active;
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
                 int pinned; } sp_wslot;
static sp_wslot   g_wslot[SP_MAX_WORKERS];   /* per-worker: the green thread it runs + when it started */
static void sp_recompute_safepoint_flag(void) {   /* PRE: g_sched_lock held */
  SP_SAFEPOINT_SET(g_stw_active || g_npreempt > 0);
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
    if (wid != sp_worker_id) sp_ev_kick(wid);   /* ourselves: we loop and re-pick */
    return;
  }
#endif
  if (wid == 0) { pthread_cond_broadcast(&g_sched_work); return; }
  if (g_wslot[wid].idle) pthread_cond_signal(&g_wslot[wid].cv);
}

static void sched_wake_main(void) {   /* PRE: sched lock held */
#ifdef SP_EV_BACKEND
  if (g_wslot[0].evfd > 0) { sched_kick_worker(0); return; }
#endif
  pthread_cond_broadcast(&g_sched_work);
}

static void sched_wake_idle_helper(void) {   /* PRE: sched lock held */
  for (int i = 1; i < sp_active_workers; i++) {
#ifdef SP_EV_BACKEND
    if (g_wslot[i].evfd > 0) { sched_kick_worker(i); return; }
#endif
    if (g_wslot[i].idle) { pthread_cond_signal(&g_wslot[i].cv); return; }
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
    if (g_wslot[i].idle) pthread_cond_signal(&g_wslot[i].cv);
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
#define SCHED_WAKE_ALL() sched_wake_all_workers(0)
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
static void sp_out_enter_locked(int wid, int how) {
  sp_stw_publish_locked();
  if (wid >= 0 && wid < SP_MAX_WORKERS) {
    sp_Fiber *root = sp_fiber_worker_root();
    g_native_nfiber[wid] = 0;
    if (sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = sp_fiber_current;
    if (root && root != sp_fiber_current) g_native_fiber[wid][g_native_nfiber[wid]++] = root;
    SP_ATOMIC_STORE(&g_native_out[wid], (unsigned char)how, __ATOMIC_RELEASE);
  }
  g_nnative++;
}
/* Back in the world (PRE: sched lock held). A collection in progress counted
   this worker as out; it becomes a parker of this epoch and waits for the
   release like one (the fibers it recorded stay valid until then: nothing
   has run on it). */
static void sp_out_leave_locked(int wid) {
  if (g_stw_active) {
    g_nnative--;
    unsigned my_epoch = g_stw_epoch;
    g_nparked++;
    if (g_nparked + g_nnative >= sp_active_workers - 1) pthread_cond_signal(&g_stw_request);
    while (g_stw_active && g_stw_epoch == my_epoch) {
      if (g_cs_help && SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) {
        SCHED_UNLOCK(); sp_cs_help_run(); SCHED_LOCK(); continue;
      }
      pthread_cond_wait(&g_stw_release, &g_sched_lock);
    }
    if (g_stw_epoch == my_epoch) g_nparked--;
  }
  else g_nnative--;
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
  if (g_nparked + g_nnative >= sp_active_workers - 1) pthread_cond_signal(&g_stw_request);
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
#define SCHED_WAKE()    ((void)0)
#define SCHED_WAKE_ALL() ((void)0)
#define SCHED_WAKE_MAIN() ((void)0)
#define sched_wake_home(w) ((void)(w))
#endif

#ifdef SP_THREADS
static void sp_safepoint_preempt(void);   /* defined after the scheduler state below */
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
  sp_gc_collect();
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
  SCHED_LOCK();
  if (g_stw_active) { sp_stw_park_locked(); SCHED_UNLOCK(); return; }
  if (!force && !sp_gc_collection_wanted()) { SCHED_UNLOCK(); return; }  /* another worker just collected */
  g_stw_active = 1;
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
  while (g_nparked + g_nnative < sp_active_workers - 1) pthread_cond_wait(&g_stw_request, &g_sched_lock);
  /* the workers out in a blocking native call: their roots were published
     on the way out, and the fibers they recorded are marked like a parked
     worker's (a worker that came back meanwhile is a parker of this epoch,
     still with its slot recorded, since nothing has run on it) */
  for (int w = 0; w < SP_MAX_WORKERS && w < sp_active_workers; w++) {
    if (!g_native_out[w]) continue;
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
  g_stw_active = 0;
  sp_recompute_safepoint_flag();   /* keep the flag set if a preempt is still pending */
  pthread_cond_broadcast(&g_stw_release);
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sched_wake_all_workers(2);   /* the idle owners sweep their own lists */
  if (sp_gc_ph_on) sp_gc_ph_barrier += sp_monotonic_now() - bt0;
  SCHED_UNLOCK();
  /* the collector's own young lists, like every released worker's */
  if (SP_ATOMIC_LOAD(&g_cs_unclaimed, __ATOMIC_RELAXED) > 0) sp_cs_owner_run(sp_worker_id);
  if (g_cs_full && sp_slab_on > 0 && sp_cs_chunks_settled(sp_worker_id)) sp_slab_release_worker(sp_worker_id);
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
    = { .guard = { [_Alignof(sp_thread) - 1] = (char)0xfd }, .t = {0} };
#define g_main_thread (g_main_thread_box.t)   /* the main thread: runs on root, fiber == NULL */
static SP_TLS sp_thread *g_current = NULL;   /* per-worker: the green thread this worker runs now */
/* Run queues (design 3.1). A worker requeues a thread it just ran onto its OWN
   local queue (g_lrq[wid]) so a yielding thread reruns on the same worker (warm
   cache); spawns and wakeups, which have no worker affinity, land on the shared
   global queue (g_grq). A worker picks local-first, then global, then steals one
   from another worker before parking. All queues are guarded by g_sched_lock --
   this gives locality and load balancing without a second lock; reducing the
   lock itself would mean reworking the off-cpu handshake (deferred, see git log).
   g_runnable is the total parked-runnable count across every queue, for the
   quiescence/deadlock predicate. */
SP_TLS int sp_worker_id = 0;       /* exported: sp_alloc.h indexes the per-worker string heaps */       /* this worker's run-queue slot (0 = main); both builds */
typedef struct { sp_thread *head, *tail; } sp_runq;
static sp_runq    g_grq;                 /* global run queue: spawned + woken threads */
static sp_runq    g_lrq[SP_MAX_WORKERS]; /* per-worker local run queues (rerun locality) */
static int        g_runnable = 0;        /* threads sitting in any run queue right now */
static int        g_nrunning = 0;        /* workers currently executing a green thread (quiescence) */
static sp_thread *g_sleepers = NULL;     /* threads parked in Kernel#sleep, woken by deadline */
static sp_thread *g_io_waiters = NULL;    /* threads parked on a fd, woken by the monitor's poll */
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
typedef struct { sp_ev_waiter *waiters; } sp_ev_slot;   /* indexed by fd: the threads' entries parked on it */
static int         g_ev_fd  = -1;
static sp_ev_slot *g_ev_tab = NULL;
static int         g_ev_cap = 0;
static unsigned long long g_ev_arms = 0, g_ev_adds = 0, g_ev_lost = 0, g_ev_timeouts = 0, g_ev_backstop = 0;
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
  if (g_wslot[wid].evfd == -2 || sp_ev_disabled()) { g_wslot[wid].evfd = -2; return 0; }
#ifdef SP_EV_EPOLL
  int fd = epoll_create1(EPOLL_CLOEXEC);
#else
  int fd = kqueue();
  if (fd >= 0) { int fl = fcntl(fd, F_GETFD); if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC); }
#endif
  if (fd < 0) { g_wslot[wid].evfd = -2; return 0; }
  if (pipe(g_wslot[wid].kick) != 0) { close(fd); g_wslot[wid].evfd = -2; return 0; }
  for (int i = 0; i < 2; i++) {
    int fl = fcntl(g_wslot[wid].kick[i], F_GETFL);
    if (fl >= 0) fcntl(g_wslot[wid].kick[i], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(g_wslot[wid].kick[i], F_GETFD);
    if (fl >= 0) fcntl(g_wslot[wid].kick[i], F_SETFD, fl | FD_CLOEXEC);
  }
  g_wslot[wid].evfd = fd;
  g_wslot[wid].kick_armed = 0;
  g_ev_fd = fd;   /* any set being up is what tells the monitor the backend is live */
  return 1;
}
/* Re-arm the kick pipe. One-shot like everything else in the set. */
static void sp_ev_arm_kick(int wid) {   /* PRE: sched lock held */
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
static int sp_ev_home_of(sp_thread *t) { return t->home_wid < 0 ? 0 : (int)t->home_wid; }
/* What an entry waits for: the thread's own descriptor, or one of its set */
static short sp_evw_events(sp_ev_waiter *e) { return e->idx < 0 ? e->t->io_events : e->t->io_set[e->idx].events; }
static void sp_ev_arm_fd(int fd) {   /* PRE: sched lock held */
  if (fd < 0 || fd >= g_ev_cap) return;
  /* One descriptor can be waited on by threads pinned to DIFFERENT workers --
     IO.select over a #to_io wrapper and the IO it wraps, from two threads --
     so it is armed in each of their sets, for the union of what that worker's
     waiters want. The list is one element in the ordinary case, which is why
     the quadratic shape here costs nothing. */
  int done[8]; int nd = 0;
  for (sp_ev_waiter *w = g_ev_tab[fd].waiters; w; w = w->next) {
    int h = sp_ev_home_of(w->t), seen = 0;
    for (int i = 0; i < nd; i++) if (done[i] == h) { seen = 1; break; }
    if (seen) continue;
    if (nd < 8) done[nd++] = h;
    short want = 0;
    for (sp_ev_waiter *x = g_ev_tab[fd].waiters; x; x = x->next)
      if (sp_ev_home_of(x->t) == h) want |= sp_evw_events(x);
    if (!want || g_wslot[h].evfd <= 0) continue;
    SP_STAT_ADD(g_ev_arms, 1);
    if (sp_ev_backend_arm(g_wslot[h].evfd, fd, want) == 0) continue;
    SP_STAT_ADD(g_ev_lost, 1);
  }
  return;
  {
  /* EBADF, or a descriptor the kernel will not watch (epoll refuses a regular
     file): the waiter falls back on its deadline, which is the same answer
     poll gave for an always-ready file. Counted, so SPINEL_SCHED_STATS shows
     it rather than leaving a silent gap. */
  } }
/* Returns 0 when the descriptor could not be taken into the set. The waiter is
   then on its deadline alone -- the same degradation the poll path already had
   when its own array could not grow. */
static int sp_ev_park_entry(sp_ev_waiter *e, int fd) {   /* PRE: sched lock held */
  sp_thread *t = e->t;
  if (fd < 0 || !sp_ev_worker_up(sp_ev_home_of(t))) return 0;
  if (fd >= g_ev_cap) {
    int nc = g_ev_cap ? g_ev_cap : 64;
    while (nc <= fd) nc *= 2;
    sp_ev_slot *nt = (sp_ev_slot *)realloc(g_ev_tab, sizeof(sp_ev_slot) * (size_t)nc);
    if (!nt) return 0;
    memset(nt + g_ev_cap, 0, sizeof(sp_ev_slot) * (size_t)(nc - g_ev_cap));
    g_ev_tab = nt; g_ev_cap = nc;
  }
  e->next = g_ev_tab[fd].waiters;
  g_ev_tab[fd].waiters = e;
  sp_ev_arm_fd(fd);
  return 1;
}
static int sp_ev_park(sp_thread *t, int fd) {   /* the thread's own descriptor */
  t->ev0.t = t; t->ev0.idx = -1;
  return sp_ev_park_entry(&t->ev0, fd);
}
/* Hand one readiness event to the threads waiting on that descriptor. Called
   by whichever worker's set produced it, so `only_home` filters to the threads
   that worker can actually run -- a descriptor shared by two homes is armed in
   both sets and each worker takes its own. Re-arms for whoever is left, since
   the arm is one-shot and a waiter still parked must not be stranded.
   Answers how many threads it readied. PRE: sched lock held. */
static void sp_sched_wake_for(sp_thread *t);
static void runq_requeue(sp_thread *t);
static void sp_ev_drop(sp_thread *t);
static int sp_ev_dispatch(int rfd, short rev, int only_home) {
  if (rfd < 0 || rfd >= g_ev_cap) return 0;
  int n = 0;
  for (sp_ev_waiter *e = g_ev_tab[rfd].waiters, *nx = NULL; e; e = nx) {
    nx = e->next;
    sp_thread *w = e->t;
    if (!(sp_evw_events(e) & rev)) continue;
    if (w->wait_head != &g_io_waiters) continue;
    if (only_home >= 0 && sp_ev_home_of(w) != only_home) continue;
    SP_STAT_ADD(g_mon_readied, 1);
    for (sp_thread **pp = &g_io_waiters; *pp; pp = &(*pp)->wait_next)
      if (*pp == w) { *pp = w->wait_next; break; }
    sp_timer_cancel(w);
    sp_ev_drop(w);
    w->wait_next = NULL; w->wait_head = NULL;
    w->io_revents = rev; w->io_fd = -1;
    n++;
    if (w == &g_main_thread) { w->state = SP_TH_RUNNABLE; SCHED_WAKE_ALL(); }
    else if (w->off_cpu) { w->state = SP_TH_RUNNABLE; runq_requeue(w); sp_sched_wake_for(w); }
    else w->wake_pending = 1;
  }
  sp_ev_arm_fd(rfd);
  return n;
}

/* Wait on this worker's own readiness set for up to `tmo_ms`, then dispatch
   what came back. Releases and retakes the lock around the wait. Answers how
   many threads it readied. PRE/POST: sched lock held. */
static int sp_ev_worker_wait(int wid, int tmo_ms) {
  if (g_wslot[wid].evfd <= 0) return 0;
  sp_ev_ready evs[64];
  sp_ev_arm_kick(wid);
  g_wslot[wid].ev_waiting = 1;
  int set = g_wslot[wid].evfd;
  /* the idle wait (not the per-turn zero-timeout drain) leaves the world: a
     collection raised meanwhile marks this worker's recorded fibers and does
     not wake it to park */
  int out = tmo_ms != 0 && !g_stw_active;
  if (out) sp_out_enter_locked(wid, SP_OUT_IDLE);
  SCHED_UNLOCK();
  int en = sp_ev_backend_wait(set, evs, 64, tmo_ms);
  SCHED_LOCK();
  if (out) sp_out_leave_locked(wid);
  g_wslot[wid].ev_waiting = 0;
  if (en == 0) SP_STAT_ADD(g_ev_backstop, 1);
  SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, en > 0 ? en : 0);
  int n = 0;
  for (int i = 0; i < en; i++) {
    if (evs[i].fd == g_wslot[wid].kick[0]) {
      char buf[64]; while (read(g_wslot[wid].kick[0], buf, sizeof buf) > 0) {}
      g_wslot[wid].kick_armed = 0;
      continue;
    }
    n += sp_ev_dispatch(evs[i].fd, evs[i].rev, wid);
  }
  return n;
}

/* Take a thread off its descriptors' waiter lists -- its own, and every entry
   of a set wait. Every path that unlinks a waiter from g_io_waiters goes
   through here. */
static void sp_ev_unlink(sp_ev_waiter *e, int fd) {   /* PRE: sched lock held */
  if (fd < 0 || fd >= g_ev_cap) { e->next = NULL; return; }
  for (sp_ev_waiter **pp = &g_ev_tab[fd].waiters; *pp; pp = &(*pp)->next)
    if (*pp == e) { *pp = e->next; break; }
  e->next = NULL;
}
static void sp_ev_drop(sp_thread *t) {   /* PRE: sched lock held */
  sp_ev_unlink(&t->ev0, t->io_fd);
  for (int i = 0; t->ev_set && i < t->io_nset; i++) sp_ev_unlink(&t->ev_set[i], t->io_set[i].fd);
}
/* A handle is closing: the descriptor is about to stop being ours, so drop the
   registration while the fd still names the right thing. Waiters parked on it
   keep their deadline. */
void sp_sched_ev_forget(int fd) {
  if (fd < 0) return;
  SCHED_LOCK();
  /* Wake whoever is parked on the descriptor first: a thread blocked in a
     read has no deadline, so with its registration gone and its entry
     dropped nothing would ever ready it -- `sock.close` from another thread
     left the reader waiting forever, where CRuby raises IOError in it
     (#4546). Readied here as for any readiness event, from whatever worker
     it is pinned to; the read it retries finds the handle closed and raises.
     Then the registrations go, while the number still names this descriptor. */
  if (fd < g_ev_cap && g_ev_tab[fd].waiters) sp_ev_dispatch(fd, POLLIN | POLLOUT, -1);
  for (int wid = 0; wid < SP_MAX_WORKERS; wid++)
    if (g_wslot[wid].evfd > 0) sp_ev_backend_del(g_wslot[wid].evfd, fd);
  if (fd < g_ev_cap) {
    for (sp_ev_waiter *w = g_ev_tab[fd].waiters; w; ) { sp_ev_waiter *n = w->next; w->next = NULL; w = n; }
    g_ev_tab[fd].waiters = NULL;
  }
  SCHED_UNLOCK();
}
#else
void sp_sched_ev_forget(int fd) { (void)fd; }
#endif

static unsigned   g_next_id = 1;
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
static void runq_push(sp_runq *q, sp_thread *t) {
  t->state = SP_TH_RUNNABLE; t->rq_next = NULL;
  if (sched_lat_enabled()) t->readied_at = sp_monotonic_now();
  if (q->tail) q->tail->rq_next = t; else q->head = t;
  q->tail = t; g_runnable++;
#ifdef SP_THREADS
  sp_sched_maybe_grow();   /* one execution helper per live green thread, up to the cap */
#endif
}
static sp_thread *runq_pop(sp_runq *q) {
  sp_thread *t = q->head;
  if (t) { q->head = t->rq_next; if (!q->head) q->tail = NULL; t->rq_next = NULL; g_runnable--; }
  return t;
}
/* Spawn: the shared global queue (no worker affinity yet). */
static void rq_push(sp_thread *t)           { runq_push(&g_grq, t); }
/* Rerun a thread on the worker that just ran it (locality). */
static void lrq_push(int wid, sp_thread *t) { runq_push(&g_lrq[wid], t); }
/* Requeue/wake a thread that may have already run. A STARTED thread must go
   back to its home worker's local queue -- never the global queue -- because
   its live frames cache that worker's __thread addresses (see home_wid). An
   unstarted thread has no affinity and takes the global queue. */
static void runq_requeue(sp_thread *t) {
#ifdef SP_THREADS
  if (t->home_wid >= 0) { runq_push(&g_lrq[t->home_wid], t); return; }
#endif
  runq_push(&g_grq, t);
}
/* Next thread for worker `wid`: its own queue, then the global queue, then steal
   one from another worker. Every 61st pick checks the global queue first so a
   worker that keeps refilling its own queue (e.g. a thread spawning in a loop)
   cannot starve globally-requeued (preempted / woken) work. */
static SP_TLS unsigned g_pick_tick = 0;
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
    if (best < 0 || g_wslot[i].pinned < bestn) { best = i; bestn = g_wslot[i].pinned; }
  }
  if (best < 0) return 0;
  if (bestn > 0 && g_helpers_spawned < g_worker_cap && !g_stw_active) {
    int before = g_helpers_spawned;
    sp_sched_spawn_helper();
    if (g_helpers_spawned > before) { best = g_helpers_spawned; bestn = 0; }
  }
  if (best == wid || (wid > 0 && g_wslot[wid].pinned <= bestn)) return 0;   /* here is as good */
  t->home_wid = (short)best; g_wslot[best].pinned++;
  runq_push(&g_lrq[best], t);
  sched_wake_home(best);
  return 1;
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
  t = runq_pop(&g_lrq[wid]);
  if (t) return t;
  t = runq_pop(&g_grq);
  if (t) return t;
#ifdef SP_THREADS
  for (int i = 0; i < sp_active_workers; i++) {   /* steal one from a busier worker */
    if (i == wid) continue;
    /* Only an UNSTARTED thread may be stolen: a started one is pinned to its
       home worker's TLS (home_wid) and must not resume elsewhere. Unstarted
       spawns sit at most one-per-spawner ahead of pinned reruns, so scan the
       queue rather than popping blindly. */
    sp_thread **pp = &g_lrq[i].head;
    while (*pp && (*pp)->home_wid >= 0) pp = &(*pp)->rq_next;
    if (*pp) {
      t = *pp;
      *pp = t->rq_next;
      if (g_lrq[i].tail == t) {
        g_lrq[i].tail = NULL;
        for (sp_thread *w = g_lrq[i].head; w; w = w->rq_next) g_lrq[i].tail = w;
      }
      t->rq_next = NULL; g_runnable--;
      return t;
    }
  }
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

static void reg_add(sp_thread *t) {
  t->all_prev = NULL; t->all_next = g_all;
  if (g_all) g_all->all_prev = t;
  g_all = t;
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
  else { sp_thread *t = g_all;
    while (t) { if (t != g_current) { other = 1; break; } t = t->all_next; } }
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
int sp_sched_wait_child(int pid, int *status) {
  int r;
  if (!sp_sched_other_threads_live()) {
    do { r = (int)waitpid((pid_t)pid, status, 0); } while (r < 0 && errno == EINTR);
    return r;
  }
  { double back = 0.0002;
    for (;;) {
      do { r = (int)waitpid((pid_t)pid, status, WNOHANG); } while (r < 0 && errno == EINTR);
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
  if (t->all_prev) t->all_prev->all_next = t->all_next;
  else if (g_all == t) g_all = t->all_next;
  if (t->all_next) t->all_next->all_prev = t->all_prev;
  t->all_prev = t->all_next = NULL;
}

/* GC: root every live green thread (and thus its fiber, stack roots, and
   pending result) so a fire-and-forget thread with no user reference is not
   collected mid-run. Chained ahead of whatever globals hook was installed. */
static void (*g_prev_globals_hook)(void) = NULL;
static sp_Fiber *g_main_root = NULL;   /* worker 0's root fiber: the main thread's */
static void sp_sched_globals_mark(void) {
  for (sp_thread *t = g_all; t; t = t->all_next) sp_gc_mark(t);
  /* The main thread is a static struct, not a registry entry, so nothing
     above marks what hangs off it -- and its thread-local map
     (Thread.current[:k] = v on the main thread) is a GC object. Unmarked, a
     collection freed the map while g_main_thread.tls still pointed at it,
     and the next Thread#[]= wrote into freed memory. */
  if (g_main_thread.tls) sp_gc_mark(g_main_thread.tls);
  /* The main thread runs on worker 0's root fiber, which is thread-local, so
     sp_mark_fiber_root_storage reaches it only when worker 0 collects. Its
     storage (Fiber[:k] = v, Fiber.current.storage = h at top level) is marked
     here for a collection run by any other worker. */
  if (g_main_root) {
    if (g_main_root->storage) sp_gc_mark(g_main_root->storage);
    if (g_main_root->attrs) sp_gc_mark(g_main_root->attrs);
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
  g_main_thread.state = SP_TH_RUNNING;
  g_main_thread.report_on_exception = 1;
  g_main_thread.arg = sp_box_nil();
  g_main_thread.retval = sp_box_nil();
  g_main_thread.name = sp_box_nil();
  g_current = &g_main_thread;
  g_prev_globals_hook = sp_gc_mark_globals_hook;
  sp_gc_mark_globals_hook = sp_sched_globals_mark;
#ifdef SP_THREADS
  sp_worker_id = 0;                          /* main is worker 0 */
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
static void sp_cs_enter(void) { pthread_mutex_lock(&g_cs_lock); SP_ATOMIC_FETCH_ADD(&g_cs_running, 1, __ATOMIC_RELAXED); pthread_mutex_unlock(&g_cs_lock); }
/* The owner's share, run by the released worker with the world running. */
static void sp_cs_owner_run(int wid) {
  int was = sp_gc_in_sweeper;
  sp_cs_enter();
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
  sp_cs_enter();
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
    SP_ATOMIC_FETCH_ADD(&g_cs_running, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_cs_lock);
    sp_cs_finish(sp_cs_run_tasks(CS_RUN_SWEEPER, -1));
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
static void sp_cs_start_sweepers(void) {
  { const char *o = getenv("SPINEL_GC_OWNER"); g_cs_owner_env = !(o && *o == '0'); }
  const char *e = getenv("SPINEL_GC_SWEEPERS");
  int want = e && *e ? atoi(e) : 0;
  if (want <= 0) { want = g_worker_cap > 0 ? g_worker_cap : 1; if (want > 8) want = 8; }
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
  fprintf(stderr, "%s (%s)\n", (msg && *msg) ? msg : cls, cls);
}

/* Park/wake primitives (defined below; used by join here). */
static void       sp_sched_block(sp_thread **waitlist, int defer_inject);
static sp_thread *sp_sched_wake_one(sp_thread **waitlist);
#ifdef SP_THREADS
static int        sp_sched_block_timeout(sp_thread **waitlist, double deadline, sp_mutex *mutex);
#endif

/* A finished thread's parked joiners become runnable again (the main thread,
   if it was waiting, is released by the pump's target check, not the queue). */
static void sp_thread_wake_joiners(sp_thread *t) {
  while (sp_sched_wake_one(&t->joiners)) { }
}

/* Run runnable green threads until `target` is DEAD, until the main thread is
   woken back to RUNNABLE (it had blocked on a primitive), or until the run queue
   drains. Runs on the root fiber (the main thread). */
/* Run thread t for one timeslice: transfer into its fiber until it yields back
   (parked on a wait-list, or re-queued itself via Thread.pass) or its body
   returns. On termination, publish the result/exception and wake joiners. */
/* Quiescence: nothing left to run anywhere. A main thread parked in its pump
   (#join / drain) waits on exactly this, so the worker that empties the last of
   the work wakes it. Counting RUNNING workers (not idle ones) makes this robust:
   a worker that merely wakes spuriously and re-idles never touches g_nrunning,
   so it cannot momentarily perturb the predicate the way an idle count would. */
static void sp_sched_signal_if_quiescent(void) {
  /* Only main is waiting on this. Quiescent means there is nothing for a
     helper to find, so waking the helpers here was pure cost -- and in a
     park-heavy program (every green thread blocked on I/O between hops) it
     fired on every hop, which is most of what adding workers cost (#4305). */
  if (g_nrunning == 0 && g_runnable == 0) SCHED_WAKE_MAIN();
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

static void run_thread_once(sp_thread *t) { sp_gc_wb((void*)t);   /* PRE/POST: sched lock held */
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
  g_nrunning++;
  t->state = SP_TH_RUNNING;
  t->off_cpu = 0;        /* on-cpu now: no other worker may pick it up */
  if (t->home_wid < 0) {   /* pin to this worker (TLS affinity) */
    t->home_wid = (short)sp_worker_id;
#ifdef SP_THREADS
    g_wslot[sp_worker_id].pinned++;
#endif
  }
#ifdef SP_THREADS
  /* Publish to the monitor that this worker is now running t, and when -- it uses
     this to enforce the timeslice. Nudge the monitor if it is idle so it starts
     ticking. */
  g_wslot[sp_worker_id].cur = t;
  g_wslot[sp_worker_id].since = sp_monotonic_now();
  sp_sysmon_wake();
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
  SCHED_UNLOCK();
  sp_Fiber_transfer_catch(t->fiber, at, in, &raised, &ec, &em, &eo);
  SCHED_LOCK();
  g_sched_home = NULL;
  g_current = saved;
#ifdef SP_THREADS
  g_wslot[sp_worker_id].cur = NULL;   /* no longer timing t on this worker */
  /* If the monitor flagged t but it yielded/blocked/died before reaching a poll,
     retire the request here so the flag does not stay stuck set. */
  if (t->preempt_request) { t->preempt_request = 0; g_npreempt--; sp_recompute_safepoint_flag(); }
#endif
  if (t->fiber->state == 3) {   /* the body returned (terminated) */
    t->retval = t->fiber->yielded_value;
    t->state = SP_TH_DEAD;
#ifdef SP_THREADS
    if (t->home_wid >= 0 && g_wslot[t->home_wid].pinned > 0) g_wslot[t->home_wid].pinned--;
#endif
    int do_report = 0;
    if (raised) {
      t->has_exc = 1; t->exc_cls = ec; t->exc_msg = em; t->exc_obj = eo;
      /* A SystemExit is not a thread dying badly, it is the program being
         asked to end: CRuby reports every other class here and stays silent
         for this one, then lets the exception surface at join and terminate
         with its status. Reporting it printed a scary line for an ordinary
         `exit` inside a thread. */
      do_report = t->report_on_exception &&
                  !(ec && strcmp(ec, "SystemExit") == 0);
    }
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
    sp_thread_wake_joiners(t);
    reg_remove(t);   /* collectable once no user reference remains */
    g_nrunning--;
    SCHED_WAKE_ALL();   /* wake a pump-waiting main (joining on this thread) and idle workers */
    if (do_report) { SCHED_UNLOCK(); sp_thread_report(t); SCHED_LOCK(); }
    return;
  }
  /* t yielded back and is now fully off its stack. Only NOW is it safe for
     another worker to run it, so this is where it (re-)enters the run queue:
     - BLOCKED: it parked on a wait list. If a waker raced in while it was still
       switching out, it deferred the enqueue to us (wake_pending); do it now.
     - otherwise (still RUNNING): it yielded via Thread.pass and wants to keep
       running, so requeue it. */
  t->off_cpu = 1;
  if (t->state == SP_TH_BLOCKED) {
    if (t->wake_pending) {
      t->wake_pending = 0;
      t->state = SP_TH_RUNNABLE;
      runq_requeue(t);
      sched_wake_home(t->home_wid);   /* pinned to us: our own loop re-picks it */
    }
  }
  else {
    /* Thread.pass / preempt: requeue. A started thread is pinned to its home
       worker (TLS affinity), so it lands on our own local queue TAIL -- other
       queued work still runs first, and the 61-tick global check keeps the
       global queue from starving. */
    runq_requeue(t);
    sched_wake_home(t->home_wid);   /* pinned to us: our own loop re-picks it */
  }
  g_nrunning--;
  sp_sched_signal_if_quiescent();   /* this thread blocked/passed; if nothing else runs, wake a waiting main */
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
  sp_thread *self = g_current;
  if (!self || self == &g_main_thread || !self->preempt_request) return;
  self->preempt_request = 0;
  g_npreempt--;
  sp_recompute_safepoint_flag();
  /* Inside a fiber the thread resumed (an Enumerator's), switching out
     would park that fiber, and the thread is resumed at its own. Skip this
     slice; the monitor asks again on the next one. */
  if (sp_fiber_current != self->fiber) return;
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
   runs the very thread that will wake it. At N=1 g_nrunning is 0 between runs, so
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
    if (target && target->state == SP_TH_DEAD) return;
#ifdef SP_EV_BACKEND
    if (g_wslot[0].evfd > 0) sp_ev_worker_wait(0, 0);   /* see sp_worker_main */
#endif
    /* main blocked on a Queue/Mutex and a runnable thread just woke it */
    if (g_main_thread.state == SP_TH_RUNNABLE) { g_main_thread.state = SP_TH_RUNNING; return; }
    /* Run a green thread on the main worker only at N=1. With helpers present,
       main does NOT pump general green threads: running one means transferring
       off the root and back, which leaves the root fiber's published top-level
       roots a stale snapshot a collector could later mark. Main instead waits
       and lets a helper run the work. At N=1 there are no helpers, so it must
       pump. EXCEPTION: a thread pinned to main's worker (home_wid 0 -- it first
       ran here via a Thread.pass sweep, sp_sched_pass) can run NOWHERE else:
       helpers must not resume it (TLS affinity) and stealing skips it. If one
       is requeued while main pumps (a #kill/#raise or Queue wake unblocked it),
       main is the only worker that can finish it, so run it here -- otherwise
       the pump and that thread deadlock waiting on each other. */
#ifdef SP_THREADS
    if (sp_active_workers > 1) {
      sp_thread *t = runq_pop(&g_lrq[0]);
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
       wake us; the worker that drops g_nrunning to zero with an empty queue
       broadcasts (sp_sched_signal_if_quiescent). Only when nothing runs and the
       queue is empty do we fall through -- drained, or a deadlock the caller
       observes. */
    int outstanding = (g_nrunning > 0 || g_runnable > 0);
    /* A timed wait or an I/O waiter is work that may yet become runnable, so an
       ordinary wait counts it. The exit drain does not: nothing is going to
       ask for it after main has returned. */
    if (may_wait == 1) outstanding = outstanding || g_sleepers || g_io_waiters || g_timer_pending;
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
  int n = g_runnable;
  while (n-- > 0) {
    sp_thread *t = sched_pick(sp_worker_id);
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

sp_thread *sp_Thread_spawn_fiber_at(sp_Fiber *f, sp_RbVal arg, const char *file, sp_int line) {SP_GC_ROOT_RBVAL(arg);SP_GC_ROOT(f);SP_GC_ROOT_STR(file);
  sp_thread *t = sp_Thread_spawn_fiber(f, arg);
  t->birth_file = file;
  t->birth_line = line;
  return t;
}
sp_thread *sp_Thread_spawn_fiber(sp_Fiber *f, sp_RbVal arg) {
#ifdef SP_THREADS
  if (!sp_sched_ensure_workers())
    sp_raise_cls("ThreadError", "failed to start scheduler monitor");
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
  if (g_current == &g_main_thread && g_worker_cap > 0) rq_push(t);
  else
#endif
  lrq_push(sp_worker_id, t);
  SCHED_WAKE();   /* a helper worker may be idle: hand it the new thread */
  SCHED_UNLOCK();
  return t;
}

/* Joining the current thread can never finish. Reject it before attempting to
   park, including on the main thread before the timer monitor has started. */
static void sp_thread_check_join_target(sp_thread *t) {
  if (t == g_current)
    sp_raise_cls("ThreadError", "Target thread must not be current thread");
}

/* Block the calling thread until `t` is dead. The main thread pumps the queue;
   a spawned thread parks on t's joiners and yields to the scheduler. */
static void sp_thread_await(sp_thread *t) {
  sp_thread_check_join_target(t);
  SCHED_LOCK();
  if (t->state == SP_TH_DEAD) { SCHED_UNLOCK(); return; }
  sp_thread *self = g_current;
  if (self == &g_main_thread) {
    sp_sched_pump(t, 1);
    int dead = (t->state == SP_TH_DEAD);
    SCHED_UNLOCK();
    if (!dead) sp_raise_cls("ThreadError", "deadlock detected: no runnable thread");
  }
  else {
    sp_sched_block(&t->joiners, 0);   /* parks on t's joiners; resumes once t is dead */
    SCHED_UNLOCK();
  }
}

/* CRuby: #join and #value re-raise the thread's unhandled exception in the
   joining thread. */
static void sp_thread_reraise_if_exc(sp_thread *t) {
  if (t->has_exc) sp_fiber_reraise(t->exc_cls, t->exc_msg, t->exc_obj);
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
sp_thread *sp_Thread_join_timeout(sp_thread *t, double seconds) {
  sp_thread_check_join_target(t);
  SCHED_LOCK();
  if (t->state == SP_TH_DEAD) {
    SCHED_UNLOCK();
    sp_thread_reraise_if_exc(t);
    return t;
  }
  if (!(seconds > 0)) {
    SCHED_UNLOCK();
    return NULL;   /* also catches a NaN limit */
  }

#ifdef SP_THREADS
  int woken = sp_sched_block_timeout(&t->joiners, sp_monotonic_now() + seconds, NULL);
  if (woken < 0) {
    SCHED_UNLOCK();
    sp_raise_cls("NoMemoryError", "failed to schedule thread join timeout");
  }
  SCHED_UNLOCK();
  if (!woken) return NULL;
  sp_thread_reraise_if_exc(t);
  return t;
#else
  SCHED_UNLOCK();
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
    int dead = (t->state == SP_TH_DEAD);
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

void sp_Thread_pass(void) {
  SCHED_LOCK();
  sp_thread *self = g_current;
  if (self == &g_main_thread) {
    sp_sched_pass();   /* one round-robin sweep, then main resumes (not a drain) */
    SCHED_UNLOCK();
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

sp_bool sp_Thread_alive(sp_thread *t) { return t->state != SP_TH_DEAD; }

/* Thread.report_on_exception=(v): set the default for threads spawned after.
   Thread.report_on_exception: read the default. Per-thread #report_on_exception
   reads/sets the thread's own flag. */
sp_bool sp_Thread_set_report_default(sp_bool v) { g_report_default = v ? 1 : 0; return v; }
sp_bool sp_Thread_get_report_default(void) { return g_report_default; }
sp_bool sp_Thread_set_report(sp_thread *t, sp_bool v) { t->report_on_exception = v ? 1 : 0; return v; }
sp_bool sp_Thread_get_report(sp_thread *t) { return t->report_on_exception; }

sp_thread *sp_Thread_main(void) { return &g_main_thread; }

sp_RbVal sp_Thread_get_name(sp_thread *t) { return t->name; }
sp_RbVal sp_Thread_set_name(sp_thread *t, sp_RbVal v) { sp_gc_wb((void*)t); t->name = v; return v; }

/* Thread.list enumeration: the main thread followed by every live spawned
   thread (dead ones are off the registry). The generated TU builds the array
   over these accessors since it owns sp_PolyArray. */
sp_int sp_Thread_list_count(void) {
  sp_int n = 1;   /* the main thread */
  for (sp_thread *t = g_all; t; t = t->all_next) n++;
  return n;
}
sp_thread *sp_Thread_list_at(sp_int i) {
  if (i <= 0) return &g_main_thread;
  sp_thread *t = g_all;
  for (sp_int k = 1; t && k < i; k++) t = t->all_next;
  return t ? t : &g_main_thread;
}

/* #status: "run" while runnable/running, "sleep" while blocked, false when it
   finished normally, nil when it died with an unhandled exception. */
sp_RbVal sp_Thread_status(sp_thread *t) {
  switch (t->state) {
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

/* min(online cores, SPINEL_WORKERS); the env var overrides the autodetect. */
static int sp_worker_count(void) {
  const char *e = getenv("SPINEL_WORKERS");
  int n;
  if (e && *e) { n = atoi(e); if (n < 1) n = 1; }
  else { long c = sysconf(_SC_NPROCESSORS_ONLN); n = (c > 0) ? (int)c : 1; }
  if (n > SP_MAX_WORKERS) n = SP_MAX_WORKERS;
  return n;
}

/* The monitor thread (sysmon, design §5). Two jobs, both off the workers' backs:
   (1) fire scheduler timers (sleep, timed I/O, and timed condition waits), and (2) enforce the
   timeslice -- when a worker has run the same green thread past the quantum, flag
   it for preemption and SIGURG the worker so it yields at its next safepoint. It
   idles on g_sysmon_cv when nothing sleeps and no worker runs a green thread, and
   otherwise ticks on a short nanosleep. It never participates in GC. */
static void *sp_sysmon_main(void *arg) {
  (void)arg;
  /* The monitor must never field a preemption signal itself. */
  sigset_t blk; sigemptyset(&blk); sigaddset(&blk, g_preempt_sig);
  pthread_sigmask(SIG_BLOCK, &blk, NULL);
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
        int busyw = 0; for (int i = 0; i < sp_active_workers; i++) if (g_wslot[i].active && g_wslot[i].cur) busyw++;
        fprintf(stderr, "[sched-lat] workers=%d busy=%d runnable=%d nrunning=%d\n", sp_active_workers, busyw, g_runnable, g_nrunning);
        { int pinned[SP_MAX_WORKERS] = {0}; int unpinned = 0;
          for (sp_thread *a = g_all; a; a = a->all_next) { if (a->home_wid >= 0) pinned[a->home_wid]++; else unpinned++; }
          fprintf(stderr, "[sched-lat] pinned per worker:"); for (int i = 0; i < sp_active_workers; i++) fprintf(stderr, " %d", pinned[i]); fprintf(stderr, "  unpinned=%d\n", unpinned); }
        sched_hist_print("mutex-wait", g_mtx_hist, g_mtx_n, g_mtx_max);
        fprintf(stderr, "[mutex-wait] fast-path locks=%llu spin-acquired=%llu\n", g_mtx_fast, g_mtx_spin);
        sched_hist_print("cv-wait", g_cv_hist, g_cv_n, g_cv_max);
        g_lrq_len_max = 0;
      }
    }
    sp_timer_flush();
    sp_timer_fire_due(now);
    double nearest = sp_timer_next_deadline();
    int npf = 1, nio = 0, busy = 0;
    /* Timeslice enforcement, on every turn: it reads the worker slots, not the
       wait lists, so it is O(workers) and does not belong behind the skip
       below. (An earlier cut jumped over its declarations, which is how `busy`
       came to be read uninitialised.) */
    for (int i = 0; i < sp_active_workers; i++) {
      sp_thread *r = g_wslot[i].active ? g_wslot[i].cur : NULL;
      if (!r) continue;
      busy = 1;
      if (!r->preempt_request && (now - g_wslot[i].since) >= SP_PREEMPT_QUANTUM) {
        r->preempt_request = 1;
        g_npreempt++;
        sp_recompute_safepoint_flag();
        pthread_kill(g_wslot[i].tid, g_preempt_sig);   /* nudge it to its next safepoint poll */
      }
    }
    /* Rebuilding the poll set walks the I/O waiters and grows with the
       population. The event backend holds descriptors in the kernel, so skip
       that walk until a deadline is due; the timer heap still fires due
       handles above without scanning parked threads. */
    int skip_walks = 0;
#ifdef SP_EV_BACKEND
    skip_walks = (g_ev_fd >= 0 && !(g_nearest != 0.0 && now >= g_nearest));
#endif
    if (skip_walks) {
      if (g_nearest != 0.0 && (nearest == 0.0 || g_nearest < nearest)) nearest = g_nearest;
    }
    else {
    /* Build the I/O poll set: slot 0 is the wake pipe (a registering thread
       writes a byte to break us out of poll early), the rest are parked fds.
       Timed I/O waiters use the same timer heap as sleeps and timed condition
       waits; this list is only for readiness registration. */
    for (sp_thread **wp = &g_io_waiters; *wp; ) {
      sp_thread *w = *wp;
      wp = &w->wait_next;
      nio++;
#ifdef SP_EV_BACKEND
      if (g_ev_fd >= 0) continue;   /* the kernel holds the interest set */
#endif
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
    g_nearest = nearest;   /* re-derived from the heap and current I/O waiters */
    }
    int have_io = (nio > 0) || (g_io_waiters != NULL);
    if (nearest == 0.0 && !busy && !have_io) {
      /* nothing to time or watch: sleep until a thread sleeps / waits on I/O /
         is picked up by a worker (a registrant signals g_sysmon_cv). */
      g_sysmon_idle = 1;
      pthread_cond_wait(&g_sysmon_cv, &g_sched_lock);
      g_sysmon_idle = 0;
    }
    else {
      /* Poll the parked fds, timing out at the quantum (while preempting), near
         the nearest sleeper deadline, or 50ms otherwise (poll returns earlier on
         fd activity or a wake-pipe byte). */
      double dt = busy ? SP_PREEMPT_TICK : (nearest != 0.0 ? nearest - now : 0.05);
      if (nearest != 0.0 && nearest - now < dt) dt = nearest - now;
      if (dt > 0.05) dt = 0.05;
      if (dt < 0.0005) dt = 0.0005;
      int tmo = (int)(dt * 1000.0); if (tmo < 1) tmo = 1;
#ifdef SP_EV_BACKEND
      if (g_ev_fd > 0) {
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
        g_sysmon_idle = 1;
        pthread_cond_timedwait(&g_sysmon_cv, &g_sched_lock, &ts);
        g_sysmon_idle = 0;
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
      SCHED_UNLOCK();
      int pr = poll(g_pfds, (nfds_t)npf, tmo);
      SCHED_LOCK();
      SP_STAT_ADD(g_mon_polls, 1); SP_STAT_ADD(g_mon_pollfds, npf);
      if (g_pfds[0].revents & POLLIN) {   /* drain the wake pipe */
        char buf[64]; while (read(g_sysmon_pipe[0], buf, sizeof buf) > 0) {}
      }
      if (pr > 0) {
        for (int i = 1; i < npf; i++) {
          if (!g_pfds[i].revents) continue;
          SP_STAT_ADD(g_mon_readied, 1);
          sp_thread *t = g_pths[i];
          if (t->wait_head != &g_io_waiters) continue;   /* unparked meanwhile (e.g. #kill) */
          for (sp_thread **pp = &g_io_waiters; *pp; pp = &(*pp)->wait_next)
            if (*pp == t) { *pp = t->wait_next; break; }
          sp_timer_cancel(t);
          t->wait_next = NULL; t->wait_head = NULL; t->io_revents = g_pfds[i].revents; t->io_fd = -1;
          if (t == &g_main_thread) { t->state = SP_TH_RUNNABLE; SCHED_WAKE_ALL(); }
          else if (t->off_cpu) { t->state = SP_TH_RUNNABLE; runq_requeue(t); sp_sched_wake_for(t); }
          else t->wake_pending = 1;
        }
      }
    }
  }
  SCHED_UNLOCK();
  return NULL;
}

static void sp_sched_unpark(sp_thread *t);   /* below */

/* Kernel#sleep, Thread.stop and Mutex#sleep park here: on g_sleepers, with a
   timer unless seconds < 0 (until #wakeup). Answers 1 when Thread#wakeup
   ended the sleep early, 0 when the time ran out, -1 when its timer could
   not be allocated (the caller raises, after any cleanup of its own). */
static int sp_sched_sleep_park(double seconds) {
  if (!g_sysmon_started) {   /* the monitor was not started: plain blocking sleep */
    if (seconds < 0.0) for (;;) pause();   /* nothing else could wake us */
    struct timespec req; req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) req.tv_nsec = 0; if (req.tv_nsec >= 1000000000L) req.tv_nsec = 999999999L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
    return 0;
  }
  SCHED_LOCK();
  sp_thread *self = g_current;
  /* Same pending-inject check as sp_sched_block: a #kill/#raise that arrived
     while we were RUNNING must fire now, not after the full sleep. */
  if (self != &g_main_thread && self->fiber && sp_fiber_inject_pending(self->fiber)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    SCHED_LOCK();
  }
  if (seconds >= 0.0 && !sp_timer_schedule(self, sp_monotonic_now() + seconds, SP_TIMER_SLEEP)) {
    SCHED_UNLOCK();
    return -1;
  }
  self->state = SP_TH_BLOCKED;
  self->off_cpu = 0;
  self->wake_pending = 0;
  self->woken = 0;
  self->wait_next = g_sleepers; self->wait_head = &g_sleepers; g_sleepers = self;
  int woken;
  if (self == &g_main_thread) {
    sp_sched_pump(NULL, 1);   /* main waits (and pumps at N=1) until the monitor wakes it */
    woken = self->woken;
    SCHED_UNLOCK();
  }
  else {
    /* Same exception-context snapshot as sp_sched_block: the symmetric transfer
       clobbers our handler stack on resume. */
    void *exc_snap = sp_exc_ctx_new();
    sp_exc_ctx_save(exc_snap);
    SCHED_UNLOCK();
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_exc_ctx_free(exc_snap);
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
  SCHED_LOCK();
  if (t->state == SP_TH_DEAD) { SCHED_UNLOCK(); sp_raise_cls("ThreadError", "killed thread"); }
  if (t->wait_head == &g_sleepers) {
    t->woken = 1;
    sp_sched_unpark(t);   /* off g_sleepers, timer cancelled */
    if (t == &g_main_thread) { t->state = SP_TH_RUNNABLE; SCHED_WAKE_ALL(); }
    else if (t->off_cpu) { t->state = SP_TH_RUNNABLE; runq_requeue(t); sp_sched_wake_for(t); }
    else t->wake_pending = 1;   /* mid-switch: its worker enqueues it */
  }
  SCHED_UNLOCK();
  return t;
}

sp_thread *sp_Thread_run(sp_thread *t) {
  sp_Thread_wakeup(t);
  sp_Thread_pass();
  return t;
}

sp_bool sp_Thread_stop_p(sp_thread *t) {
  SCHED_LOCK();
  sp_bool r = t->state == SP_TH_DEAD || t->state == SP_TH_BLOCKED;
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
      if (pr > 0) return 1; if (pr == 0) return 0; if (errno == EINTR) continue; return 0;
    }
  }
  for (;;) { int pr = poll(pp, np, 1000); if (pr > 0) return 1; if (pr == 0 || errno == EINTR) continue; return 0; }
}
static int sp_sched_wait_io_impl(int fd, short events, struct pollfd *set, int n, double timeout_s,
                                 const unsigned char *cancel) {
  if ((set ? n <= 0 : fd < 0) || !g_sysmon_started) {   /* no monitor: plain blocking poll */
    struct pollfd pf; pf.fd = fd; pf.events = events; pf.revents = 0;
    return sp_poll_plain(set ? set : &pf, set ? (nfds_t)n : 1, timeout_s);
  }
  SCHED_LOCK();
  sp_thread *self = g_current;
  /* Same pending-inject check as sp_sched_block. */
  if (self != &g_main_thread && self->fiber && sp_fiber_inject_pending(self->fiber)) {
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
  if (cancel && *cancel) { SCHED_UNLOCK(); return 1; }
  sp_ev_waiter *es = NULL;
  if (set) {
    es = (sp_ev_waiter *)malloc(sizeof(sp_ev_waiter) * (size_t)n);
    if (!es) { SCHED_UNLOCK(); return sp_poll_plain(set, (nfds_t)n, timeout_s); }
    for (int i = 0; i < n; i++) { es[i].t = self; es[i].idx = i; es[i].next = NULL; }
  }
  if (timeout_s >= 0.0 &&
      !sp_timer_schedule(self, sp_monotonic_now() + timeout_s, SP_TIMER_IO)) {
    free(es);
    SCHED_UNLOCK();
    return sp_poll_plain(set ? set : &(struct pollfd){.fd = fd, .events = events},
                         set ? (nfds_t)n : 1, timeout_s);
  }
  if (set) { self->io_fd = -1; self->io_events = 0; self->io_set = set; self->io_nset = n; self->ev_set = es; }
  else { self->io_fd = fd; self->io_events = events; }
  self->io_revents = 0;
  self->state = SP_TH_BLOCKED;
  self->off_cpu = 0;
  self->wake_pending = 0;
  self->wait_next = g_io_waiters; self->wait_head = &g_io_waiters; g_io_waiters = self;
  SP_STAT_ADD(g_mon_regs, 1);
#ifdef SP_EV_BACKEND
  int registered = 1;
  if (set) { for (int i = 0; i < n; i++) if (!sp_ev_park_entry(&self->ev_set[i], set[i].fd)) registered = 0; }
  else registered = sp_ev_park(self, fd);
  if (registered) {
    /* Registered: the kernel holds the interest set, so the monitor needs no
       word about the descriptor -- only about a deadline that moved earlier.
       That is what takes the self-pipe write off the park path.

       The idle check is NOT part of that saving and must not be folded into
       it. Timer scheduling signals only when this deadline is the new
       earliest, and a monitor asleep on its condvar is woken by nothing else:
       park with a deadline LATER than a stale g_nearest, while it sleeps, and
       it sleeps through every event that follows. That is a whole-scheduler
       stall, and it is what the first cut of this did -- 16 of 16 threads
       waiting out their select timeout, about one run in twenty. */
    if (g_sysmon_idle) sp_sysmon_wake();
  }
  else
#endif
  {
    sp_sysmon_wake();   /* let the monitor rebuild its poll set */
  }
  if (self == &g_main_thread) {
    sp_sched_pump(NULL, 1);   /* main waits (and pumps at N=1) until the monitor wakes it */
    int rev = self->io_revents; self->io_revents = 0; self->io_fd = -1;
    if (set) { free(self->ev_set); self->ev_set = NULL; self->io_set = NULL; self->io_nset = 0; }
    SCHED_UNLOCK();
    return rev ? 1 : 0;
  }
  /* Same exception-context snapshot as sp_sched_block/sleep: the symmetric
     transfer clobbers our handler stack on resume. */
  void *exc_snap = sp_exc_ctx_new();
  sp_exc_ctx_save(exc_snap);
  SCHED_UNLOCK();
  sp_sched_switch_out();
  sp_exc_ctx_load(exc_snap);
  sp_exc_ctx_free(exc_snap);
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
int sp_sched_wait_io_set(struct pollfd *set, int n, double timeout_s) {
  if (n == 1) return sp_sched_wait_io_impl(set[0].fd, set[0].events, NULL, 0, timeout_s, NULL);
  return sp_sched_wait_io_impl(-1, 0, set, n, timeout_s, NULL);
}

/* A helper worker: adopt its native stack as a per-worker root fiber, then pull
   runnable green threads off the GRQ forever. It parks at the GC barrier when a
   collection is in progress and exits when main signals shutdown at drain. */
static void *sp_worker_main(void *arg) {
  int wid = (int)(intptr_t)arg;
  sp_worker_id = wid;          /* TLS: read only by this worker */
  sp_fiber_worker_init();
  SCHED_LOCK();
  g_wslot[wid].tid = pthread_self();   /* publish under the lock; the monitor reads it there */
  g_wslot[wid].active = 1;
  for (;;) {
    if (g_stw_active) { sp_stw_park_locked(); continue; }
    if (g_shutdown) break;
#ifdef SP_EV_BACKEND
    /* Drain what is ready before picking, every turn. A worker with runnable
       work never reaches the blocking wait below, and its OWN set is the only
       place its parked threads are delivered from -- so without this a busy
       worker starves them until it happens to idle. That showed as a
       ping-pong that ran at full speed on one worker and fell to a fifth of it
       on four, run to run. One zero-timeout wait per scheduling turn is what
       every netpoll scheduler pays for the same reason. */
    if (g_wslot[wid].evfd > 0) sp_ev_worker_wait(wid, 0);
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
    g_wslot[wid].idle = 1;
    sp_out_enter_locked(wid, SP_OUT_IDLE);
    pthread_cond_wait(&g_wslot[wid].cv, &g_sched_lock);   /* woken for work meant for us, or shutdown */
    sp_out_leave_locked(wid);
    g_wslot[wid].idle = 0;
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
  /* main is worker 0 and the last slot belongs to the sweeper threads */
  if (g_worker_cap > SP_MAX_WORKERS - 2) g_worker_cap = SP_MAX_WORKERS - 2;
  /* Install the preemption signal handler before any worker can be targeted.
     SA_RESTART so an in-flight library syscall resumes rather than failing with
     EINTR -- the yield itself is cooperative (at the next safepoint poll), the
     signal only nudges the worker there. Resolve and pin g_preempt_sig here,
     before the monitor reads it through the pthread_create edge. */
  g_preempt_sig = sp_resolve_preempt_signal();
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
  if (pipe(g_sysmon_pipe) == 0) {
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
  sp_active_workers = g_helpers_spawned + 1;   /* participants: main (0) + helpers */
}

/* Grow the helper pool toward one execution worker per live green thread, capped
   at g_worker_cap. Called under the lock wherever a thread becomes runnable; a
   no-op once at the cap or while a collection is reading the participant count
   (we never change it mid-STW). This turns "SPINEL_WORKERS=N" into up to N real
   parallel execution threads while never starting a helper the workload has no
   runnable thread for -- a lone background thread brings up exactly one. */
static void sp_sched_maybe_grow(void) {
  if (!g_workers_started || g_stw_active) return;
  int live = g_runnable + g_nrunning;
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
  if (self != &g_main_thread && self->fiber && sp_fiber_inject_pending(self->fiber)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();   /* raises; does not return */
    SCHED_LOCK();
  }
  self->state = SP_TH_BLOCKED;
  self->off_cpu = 0;         /* still on-cpu until our worker confirms the switch-out */
  self->wake_pending = 0;
  /* Append at the TAIL: sp_sched_wake_one takes the head, so the list is a
     FIFO and a waiter is served in the order it arrived. Pushed at the head
     it was a LIFO -- the newest waiter woke first -- and under sustained
     contention an early waiter never reached the front: campfire's DB-pool
     condvar and fragment-cache mutexes held requests for seconds at p99 (a
     2.9 s mutex wait, an 8 s condvar wait at 64 connections) while the median
     was 4 ms. The walk is O(waiters) under the scheduler lock, which is what
     one park already costs. */
  self->wait_head = waitlist;   /* so #kill/#raise can unlink it */
  if (self->repark_front) {   /* a mutex waiter that lost the race after its wake: keep its turn */
    self->repark_front = 0;
    self->wait_next = *waitlist; *waitlist = self;
  }
  else {
    self->wait_next = NULL;
    sp_thread **pp = waitlist; while (*pp) pp = &(*pp)->wait_next; *pp = self;
  }
  if (self == &g_main_thread) {
    sp_sched_pump(NULL, 1);   /* returns (lock held) once a waker marks main RUNNABLE */
    if (self->state != SP_TH_RUNNING) {
      SCHED_UNLOCK();
      sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
    }
  }
  else {
    /* The symmetric fiber transfer's exc bookkeeping clobbers this thread's
       handler stack when root resumes from the block, so snapshot it here and
       restore it on wake -- otherwise a #raise/#kill delivered while blocked
       would find an empty handler stack and escape unhandled. */
    void *exc_snap = sp_exc_ctx_new();
    sp_exc_ctx_save(exc_snap);
    if (defer_inject) sp_fiber_defer_inject();
    SCHED_UNLOCK();   /* drop the lock across the transfer (we run no metadata while parked) */
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_exc_ctx_free(exc_snap);
    /* resumed: a pending #kill/#raise fires here, in this thread's context (lock
       not held), so its ensure/rescue blocks unwind on its own stack. On the
       normal resume we re-take the lock so the caller continues holding it. */
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
}

#ifdef SP_THREADS
/* A timed wait reserves its timer and publishes the waiter under the scheduler
   lock, preserving wake-vs-park atomicity. If mutex is non-NULL, it is checked
   and released as part of parking (ConditionVariable#wait); interruption stays
   pending until the caller reacquires it. Other waiters such as Queue#pop pass
   NULL. Returns -1 if timer storage could not be allocated,
   0 if the deadline expired, or 1 if another event woke the thread. The caller
   holds the scheduler lock on return. */
static int sp_sched_block_timeout(sp_thread **waitlist, double deadline, sp_mutex *mutex) {
  sp_thread *self = g_current;
  if (mutex && SP_ATOMIC_LOAD(&mutex->owner, __ATOMIC_SEQ_CST) != self) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  if (self != &g_main_thread && self->fiber && sp_fiber_inject_pending(self->fiber)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
  self->timer_expired = 0;
  if (!sp_timer_schedule(self, deadline, SP_TIMER_WAIT)) return -1;

  if (mutex) {
    SP_ATOMIC_STORE(&mutex->owner, NULL, __ATOMIC_SEQ_CST);
    if (mutex->waiters) sp_sched_wake_one(&mutex->waiters);
  }
  self->state = SP_TH_BLOCKED;
  self->off_cpu = 0;
  self->wake_pending = 0;
  self->wait_head = waitlist;
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
  if (self == &g_main_thread) {
    sp_sched_pump(NULL, 1);
    if (self->state != SP_TH_RUNNING) {
      SCHED_UNLOCK();
      sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
    }
  }
  else {
    void *exc_snap = sp_exc_ctx_new();
    sp_exc_ctx_save(exc_snap);
    if (mutex) sp_fiber_defer_inject();
    SCHED_UNLOCK();
    sp_sched_switch_out();
    sp_exc_ctx_load(exc_snap);
    sp_exc_ctx_free(exc_snap);
    if (!mutex) sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
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
  if (t == &g_main_thread) { t->state = SP_TH_RUNNABLE; SCHED_WAKE_ALL(); return t; }  /* broadcast: a signal could wake a helper instead of main */
  if (t->off_cpu) { t->state = SP_TH_RUNNABLE; runq_requeue(t); }   /* fully parked: enqueue now */
  else { t->wake_pending = 1; }   /* still switching out: its worker enqueues it once off-cpu */
  sp_sched_wake_for(t);   /* wake an idle worker to run it (all of them if t is pinned) */
  return t;
}

/* Remove a parked thread from whatever wait list it sits on (for #kill/#raise). */
static void sp_sched_unpark(sp_thread *t) {
  if (!t->wait_head) return;
#ifdef SP_THREADS
  sp_timer_cancel(t);
#endif
#ifdef SP_EV_BACKEND
  if (t->wait_head == &g_io_waiters) sp_ev_drop(t);
#endif
  for (sp_thread **pp = t->wait_head; *pp; pp = &(*pp)->wait_next)
    if (*pp == t) { *pp = t->wait_next; break; }
  t->wait_next = NULL;
  t->wait_head = NULL;
}

#ifdef SP_THREADS
static void sp_sched_timer_expire(sp_sched_timer *timer, double now) {
  (void)now;
  sp_thread *t = timer->thread;
  if (!t->wait_head) return;
  if (timer->kind == SP_TIMER_SLEEP && t->wait_head != &g_sleepers) return;
  if (timer->kind == SP_TIMER_IO && t->wait_head != &g_io_waiters) return;

  if (timer->kind == SP_TIMER_WAIT) t->timer_expired = 1;
  if (timer->kind == SP_TIMER_IO) {
#ifdef SP_EV_BACKEND
    SP_STAT_ADD(g_ev_timeouts, 1);
#endif
  }
  sp_sched_unpark(t);
  if (timer->kind == SP_TIMER_IO) {
    t->io_revents = 0;
    t->io_fd = -1;
  }
  if (t == &g_main_thread) {
    t->state = SP_TH_RUNNABLE;
    SCHED_WAKE_ALL();
  }
  else if (t->off_cpu) {
    t->state = SP_TH_RUNNABLE;
    runq_requeue(t);
    sp_sched_wake_for(t);
  }
  else {
    t->wake_pending = 1;
  }
}
#endif

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
  if (t->state == SP_TH_DEAD) { SCHED_UNLOCK(); return; }   /* raced with its death: no-op */
  /* #kill/#raise on the main thread from a sibling: main runs on the root
     fiber (t->fiber == NULL), which has no inject delivery points, so this is
     unsupported -- no-op rather than a NULL deref, matching the self-kill case
     above. (CRuby delivers to main; see the Thread row in docs/limitations.md.) */
  if (t == &g_main_thread || !t->fiber) { SCHED_UNLOCK(); return; }
  if (is_kill) sp_fiber_set_kill_inject(t->fiber);
  else sp_fiber_set_raise_inject(t->fiber, cls, msg, obj);
  if (t->state == SP_TH_BLOCKED) {
    sp_sched_unpark(t);
    /* runq_requeue, not rq_push: a STARTED thread is pinned to its home
       worker's TLS (home_wid) and must not resume on another worker. */
    if (t->off_cpu) { t->state = SP_TH_RUNNABLE; runq_requeue(t); sp_sched_wake_for(t); }
    else { t->wake_pending = 1; }   /* mid-switch: its worker enqueues it (see run_thread_once) */
  }
  /* RUNNABLE threads are already queued; the inject fires when they run. */
  SCHED_UNLOCK();
}

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

static int sp_queue_push_impl(sp_queue *q, sp_RbVal v, int non_block,
                              int timed, double seconds) { sp_gc_wb((void*)q);
  /* On a full SizedQueue, block until a #pop frees a slot. Root v across the
     block: it lives in this (possibly suspended) frame, and the parking
     thread's saved roots only cover the shadow stack. */
  SP_GC_ROOT_RBVAL(v);
  SP_GC_ROOT(q);
  /* A negative timeout is already expired (CRuby): no ArgumentError. */
  double deadline = timed ? sp_monotonic_now() + seconds : 0.0;
  SCHED_LOCK();
  for (;;) {
    if (q->closed) { SCHED_UNLOCK(); sp_raise_cls("ClosedQueueError", "queue closed"); }
    if (q->max <= 0 || q->len < q->max) break;
    if (non_block) { SCHED_UNLOCK(); sp_raise_cls("ThreadError", "queue full"); }
    if (timed && !(seconds > 0.0)) { SCHED_UNLOCK(); return 0; }
    if (timed) {
#ifdef SP_THREADS
      if (!g_sysmon_started) {
        /* Before the first Thread, no green thread can free a slot. There is
           no monitor to fire a timer, so wait directly and check the queue. */
        SCHED_UNLOCK();
        double remaining = deadline - sp_monotonic_now();
        if (remaining > 0.0) sp_sched_sleep(remaining);
        SCHED_LOCK();
        if (q->max > 0 && q->len >= q->max) { SCHED_UNLOCK(); return 0; }
      }
      else {
        int woken = sp_sched_block_timeout(&q->push_waiters, deadline, NULL);
        if (woken < 0) {
          SCHED_UNLOCK();
          sp_raise_cls("NoMemoryError", "failed to schedule queue timeout");
        }
        if (!woken) { SCHED_UNLOCK(); return 0; }
      }
#else
      SCHED_UNLOCK();
      double remaining = deadline - sp_monotonic_now();
      if (remaining > 0.0) sp_sleep((sp_float)remaining);
      SCHED_LOCK();
      if (q->max > 0 && q->len >= q->max) { SCHED_UNLOCK(); return 0; }
#endif
    }
    else sp_sched_block(&q->push_waiters, 0); /* releases+reacquires lock around transfer */
  }
  if (q->len == q->cap) {
    sp_int nc = q->cap * 2;
    sp_RbVal *nb = (sp_RbVal *)malloc(sizeof(sp_RbVal) * nc);
    if (!nb) { SCHED_UNLOCK(); sp_raise_cls("NoMemoryError", "failed to grow queue"); }
    for (sp_int i = 0; i < q->len; i++) nb[i] = q->buf[(q->head + i) % q->cap];
    free(q->buf);
    q->buf = nb; q->cap = nc; q->head = 0;
  }
  q->buf[(q->head + q->len) % q->cap] = v;
  q->len++;
  sp_sched_wake_one(&q->pop_waiters);   /* hand the new value to a waiting popper */
  SCHED_UNLOCK();
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

sp_RbVal sp_Queue_pop(sp_queue *q) {
  /* Block until an element is available. A closed, drained queue returns nil
     rather than blocking forever (CRuby behaviour). */
  SP_GC_ROOT(q);
  SCHED_LOCK();
  while (q->len == 0) {
    if (q->closed) { SCHED_UNLOCK(); return sp_box_nil(); }
    sp_sched_block(&q->pop_waiters, 0);
  }
  sp_RbVal v = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  if (q->max > 0) sp_sched_wake_one(&q->push_waiters);   /* a slot freed up */
  SCHED_UNLOCK();
  return v;
}

sp_RbVal sp_Queue_pop_timeout(sp_queue *q, double seconds) {
  SP_GC_ROOT(q);
  /* A negative timeout is already expired (CRuby): no ArgumentError. */
  double deadline = sp_monotonic_now() + seconds;
  SCHED_LOCK();
  while (q->len == 0) {
    if (q->closed || !(seconds > 0.0)) { SCHED_UNLOCK(); return sp_box_nil(); }
#ifdef SP_THREADS
    if (!g_sysmon_started) {
      /* Before the first Thread, no green thread can add an item. */
      SCHED_UNLOCK();
      double remaining = deadline - sp_monotonic_now();
      if (remaining > 0.0) sp_sched_sleep(remaining);
      SCHED_LOCK();
      if (q->len == 0) { SCHED_UNLOCK(); return sp_box_nil(); }
    }
    else {
      int woken = sp_sched_block_timeout(&q->pop_waiters, deadline, NULL);
      if (woken < 0) {
        SCHED_UNLOCK();
        sp_raise_cls("NoMemoryError", "failed to schedule queue timeout");
      }
      if (!woken) { SCHED_UNLOCK(); return sp_box_nil(); }
    }
#else
    SCHED_UNLOCK();
    double remaining = deadline - sp_monotonic_now();
    if (remaining > 0.0) sp_sleep((sp_float)remaining);
    SCHED_LOCK();
    if (q->len == 0) { SCHED_UNLOCK(); return sp_box_nil(); }
#endif
  }
  sp_RbVal v = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  if (q->max > 0) sp_sched_wake_one(&q->push_waiters);
  SCHED_UNLOCK();
  return v;
}

sp_RbVal sp_Queue_pop_nb(sp_queue *q) {
  /* Queue#pop(truthy): no_wait. Raise ThreadError on an empty queue, return
     the value otherwise. A closed queue returns nil. */
  SCHED_LOCK();
  if (q->len == 0) {
    if (q->closed) { SCHED_UNLOCK(); return sp_box_nil(); }
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "queue empty");
  }
  sp_RbVal v = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  if (q->max > 0) sp_sched_wake_one(&q->push_waiters);
  SCHED_UNLOCK();
  return v;
}

sp_int  sp_Queue_size(sp_queue *q)   { SCHED_LOCK(); sp_int n = q->len;       SCHED_UNLOCK(); return n; }
sp_bool sp_Queue_empty(sp_queue *q)  { SCHED_LOCK(); sp_bool e = q->len == 0;  SCHED_UNLOCK(); return e; }
sp_int  sp_Queue_max(sp_queue *q)    { SCHED_LOCK(); sp_int m = q->max;        SCHED_UNLOCK(); return m; }
sp_bool sp_Queue_closed(sp_queue *q) { SCHED_LOCK(); sp_bool c = q->closed != 0; SCHED_UNLOCK(); return c; }
/* Threads parked in #pop, plus in #push on a full SizedQueue. */
sp_int  sp_Queue_num_waiting(sp_queue *q) {
  SCHED_LOCK();
  sp_int n = 0;
  for (sp_thread *t = q->pop_waiters; t; t = t->wait_next) n++;
  for (sp_thread *t = q->push_waiters; t; t = t->wait_next) n++;
  SCHED_UNLOCK();
  return n;
}
void     sp_Queue_clear(sp_queue *q)  {
  SCHED_LOCK();
  q->head = q->len = 0;
  if (q->max > 0) while (sp_sched_wake_one(&q->push_waiters)) { }   /* all slots free */
  SCHED_UNLOCK();
}

void sp_Queue_close(sp_queue *q) {
  SCHED_LOCK();
  q->closed = 1;
  /* wake every blocked popper (they return nil) and pusher (they raise) */
  while (sp_sched_wake_one(&q->pop_waiters)) { }
  while (sp_sched_wake_one(&q->push_waiters)) { }
  SCHED_UNLOCK();
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
  SCHED_LOCK();
  /* the owner re-entering its own Monitor just goes deeper */
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self && m->reentrant) { m->depth++; SCHED_UNLOCK(); return; }
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self) { SCHED_UNLOCK(); sp_raise_cls("ThreadError", "deadlock; recursive locking"); }
  expect = NULL;
  if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    { SCHED_UNLOCK(); return; }
  /* Count ourselves BEFORE the last look at `owner`: an unlocker that clears
     it after this point re-reads the count and finds us. */
  m->nwaiters++;
  SP_ATOMIC_FENCE(__ATOMIC_SEQ_CST);
  expect = NULL;
  if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    m->nwaiters--;                           /* released under us; took it instead */
    SCHED_UNLOCK();
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
    sp_sched_block(&m->waiters, 0);
    expect = NULL;
    if (SP_ATOMIC_CAS(&m->owner, &expect, self, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    self->repark_front = 1;   /* re-park at the head (sp_sched_block honours this) */
  }
  if (mt0 > 0) sched_hist_add(g_mtx_hist, &g_mtx_n, &g_mtx_max, (sp_monotonic_now() - mt0) * 1e6);
  m->nwaiters--;
  SCHED_UNLOCK();
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
      SCHED_LOCK();
      if (m->waiters) sp_sched_wake_one(&m->waiters);   /* it takes the mutex itself, or re-parks */
      SCHED_UNLOCK();
      return;
    }
  }
  SCHED_LOCK();
  if (m->depth > 0 && SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) == self) { m->depth--; SCHED_UNLOCK(); return; }
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != self) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  if (m->waiters) sp_sched_wake_one(&m->waiters);
  SCHED_UNLOCK();
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

sp_RbVal sp_CondVar_wait(sp_condvar *cv, sp_mutex *m) {
  time_t beg = time(NULL);
  /* Release the mutex and park on the CV atomically under the one lock, so a
     concurrent #signal cannot slip in between and be lost. The mutex unlock is
     inlined (its hand-off + ownership check) since sp_Mutex_unlock would take
     the lock again on its own. */
  SCHED_LOCK();
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  if (g_current != &g_main_thread && g_current->fiber &&
      sp_fiber_inject_pending(g_current->fiber)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  if (m->waiters) sp_sched_wake_one(&m->waiters);
  double ct0 = sched_lat_enabled() ? sp_monotonic_now() : 0;
  sp_sched_block(&cv->waiters, 1);              /* park (drops+retakes the lock) */
  if (ct0 > 0) sched_hist_add(g_cv_hist, &g_cv_n, &g_cv_max, (sp_monotonic_now() - ct0) * 1e6);
  SCHED_UNLOCK();
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
  if (m->waiters) sp_sched_wake_one(&m->waiters);
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

sp_RbVal sp_CondVar_wait_timeout(sp_condvar *cv, sp_mutex *m, double seconds) {
  /* CRuby checks the interval before it lets go of the mutex */
  if (seconds < 0.0) sp_raise_cls("ArgumentError", "time interval must not be negative");
  if (!(seconds > 0.0)) return sp_CondVar_wait_nb(cv, m);
#ifdef SP_THREADS
  /* With no spawned green threads there is no monitor yet. Release the mutex
     and sleep directly; the main thread is the only possible signaler in this
     state, so there cannot be a signal to lose, and the wait always times out. */
  if (!g_sysmon_started) {
    sp_CondVar_wait_timeout_blocking(m, seconds);
    return sp_box_nil();
  }
  time_t beg = time(NULL);
  SCHED_LOCK();
  double ct0 = sched_lat_enabled() ? sp_monotonic_now() : 0;
  int woken = sp_sched_block_timeout(&cv->waiters, sp_monotonic_now() + seconds, m);
  if (woken < 0) {
    SCHED_UNLOCK();
    sp_raise_cls("NoMemoryError", "failed to schedule condition variable timeout");
  }
  if (ct0 > 0) sched_hist_add(g_cv_hist, &g_cv_n, &g_cv_max, (sp_monotonic_now() - ct0) * 1e6);
  SCHED_UNLOCK();
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
sp_RbVal sp_CondVar_wait_nb(sp_condvar *cv, sp_mutex *m) {
  (void)cv;
  SCHED_LOCK();
  if (SP_ATOMIC_LOAD(&m->owner, __ATOMIC_SEQ_CST) != g_current) {
    SCHED_UNLOCK();
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  }
  if (g_current != &g_main_thread && g_current->fiber &&
      sp_fiber_inject_pending(g_current->fiber)) {
    SCHED_UNLOCK();
    sp_fiber_fire_inject_if_pending();
    SCHED_LOCK();
  }
  sp_fiber_defer_inject();
  SP_ATOMIC_STORE(&m->owner, NULL, __ATOMIC_SEQ_CST);
  if (m->waiters) sp_sched_wake_one(&m->waiters);
  SCHED_UNLOCK();
  sp_Mutex_lock(m);
  sp_fiber_undefer_inject();
  sp_fiber_fire_inject_if_pending();
  return sp_box_nil();
}

void sp_CondVar_signal(sp_condvar *cv)    { SCHED_LOCK(); sp_sched_wake_one(&cv->waiters);            SCHED_UNLOCK(); }
void sp_CondVar_broadcast(sp_condvar *cv) { SCHED_LOCK(); while (sp_sched_wake_one(&cv->waiters)) { }  SCHED_UNLOCK(); }

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
    switch (t->state) {
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
