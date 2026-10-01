/* sp_sched.h -- cooperative M:N thread scheduler, Phase 0 (N=1).
 *
 * A Ruby Thread is a green thread (sp_thread) wrapping an sp_Fiber that the
 * codegen builds exactly like a Fiber.new block (so all the capture/self/cell
 * machinery is shared); the block's result lands in fiber->yielded_value. The
 * main program runs on the root fiber and IS the scheduler hub: it executes
 * top-level code directly and, whenever it blocks (#join / #value /
 * Thread.pass) or at program exit (drain), it pumps a FIFO run queue of
 * runnable green threads. A spawned thread yields by transferring back to the
 * root fiber, where the pump loop -- or the fiber trampoline, on termination --
 * resumes.
 *
 * With a single OS worker there is no concurrent heap mutation, so the GC and
 * allocator are untouched. This is the Phase 0 cooperative core of
 * docs/thread-mn-design.md; N>1 parallelism, preemption, and the
 * Mutex/Queue/ConditionVariable primitives build on this scaffolding later.
 */
#ifndef SP_SCHED_H
#define SP_SCHED_H

#include <poll.h>
#include "sp_fiber.h"

typedef enum { SP_TH_RUNNABLE, SP_TH_RUNNING, SP_TH_BLOCKED, SP_TH_DEAD } sp_thread_state;

/* A thread's membership in one descriptor's waiter list. idx -1 names the
   thread's own io_fd / io_events; idx >= 0 names io_set[idx]. */
typedef struct sp_ev_waiter {
  struct sp_thread     *t;
  int                   idx;
  struct sp_ev_waiter  *next;
} sp_ev_waiter;

typedef struct sp_sched_timer sp_sched_timer;

typedef struct sp_thread {
  sp_Fiber         *fiber;       /* the green thread's coroutine; NULL for the main thread (root) */
  sp_Fiber         *at;          /* the Fiber it resumed and stopped inside, or NULL when it
                                    stopped in its own: run_thread_once switches back there */
  sp_RbVal          arg;         /* Thread.new(arg) -> the block's first param, on first run */
  sp_RbVal          retval;      /* block result (copied from fiber->yielded_value at death) */
  sp_RbVal          name;        /* #name / #name= (a string or nil) */
  const char       *birth_file;  /* creation site for #inspect (#3126); rodata */
  sp_int           birth_line;
  sp_thread_state   state;
  int               has_exc;     /* body left an unhandled exception (re-raised at #join/#value) */
  const char       *exc_cls, *exc_msg;
  void             *exc_obj;
  unsigned char     report_on_exception;
  unsigned char     off_cpu;     /* set by the worker once this thread has fully switched off its
                                    stack; a waker may only enqueue it once off_cpu, otherwise it
                                    could be run on a second worker mid-context-switch */
  unsigned char     wake_pending; /* a wake arrived while still on-cpu; the worker enqueues it */
  unsigned char     repark_front;  /* next sp_sched_block puts this thread at the HEAD of the wait
                                      list: a mutex waiter woken but beaten to the exchange keeps
                                      its place in the arrival order */
  unsigned char     preempt_request; /* sysmon set this thread over its timeslice; it yields at its
                                        next safepoint poll (cooperative preemption, §5) */
  short             home_wid;      /* worker this thread FIRST ran on, -1 before its first run.
                                      A started green thread is pinned there: its live C frames
                                      hold compiler-cached addresses of that worker's __thread
                                      data (GC shadow stack, exception stack, ...), so resuming
                                      it on another worker corrupts both workers' TLS. */
  double            readied_at;  /* SPINEL_SCHED_STATS=2: when it was last put on a run queue */
  struct sp_thread *rq_next;     /* run-queue link while RUNNABLE */
  struct sp_thread *joiners;     /* threads parked in #join/#value on this one */
  sp_ev_waiter      ev0;         /* its entry in the per-DESCRIPTOR waiter list for io_fd: a
                                    readiness event names an fd, and every thread parked on
                                    that fd is woken from it (#4306) */
  sp_ev_waiter     *ev_set;      /* one entry per descriptor of a SET wait (IO.select over
                                    several handles), heap-allocated for the wait's duration */
  struct pollfd    *io_set;      /* that set: fd + events per entry, the caller's array; io_fd
                                    is -1 while it is in use (#4528) */
  int               io_nset;
  struct sp_thread *wait_next;   /* link within the wait list it is parked on */
  struct sp_thread **wait_head;  /* head of that wait list, so #kill/#raise can unpark it */
  struct sp_thread *all_next, *all_prev;  /* registry of live threads (GC roots) */
  void             *tls;         /* thread-local storage (Thread#[] / #[]=); lazily allocated */
  sp_sched_timer   *timer;       /* active scheduler timer for this wait, if any */
  unsigned char     timer_expired; /* set when a generic timed wait reaches its deadline */
  unsigned char     woken;         /* Thread#wakeup ended its sleep early */
  int               io_fd;       /* fd this thread is parked on for I/O (-1 = none, scheduler-aware I/O) */
  short             io_events;   /* poll events it is waiting for (POLLIN/POLLOUT) */
  short             io_revents;  /* poll result the monitor delivered when the fd became ready */
  unsigned          id;
} sp_thread;

/* Called once from main()'s prologue (on the root fiber, after sp_tu_init) when
   the program uses threads. Adopts the running context as the main thread and
   chains a GC mark hook that roots every live green thread. */
void       sp_sched_init(void);

/* Thread.new { ... }: wrap a fiber (built by the codegen via emit_fiber_new) in
   a green thread and enqueue it RUNNABLE. It runs the next time the current
   thread yields, or at drain. Returns the thread (boxed SP_BUILTIN_THREAD). */
sp_thread *sp_Thread_spawn_fiber(sp_Fiber *f, sp_RbVal arg);
sp_thread *sp_Thread_spawn_fiber_at(sp_Fiber *f, sp_RbVal arg, const char *file, sp_int line);

/* #join: block until the thread has finished, re-raise its unhandled exception
   in the caller, then return the thread. #value: same, but return its result. */
sp_thread *sp_Thread_join(sp_thread *t);
sp_RbVal   sp_Thread_value(sp_thread *t);

sp_thread *sp_Thread_kill(sp_thread *t);  /* #kill / #exit / #terminate */
sp_thread *sp_Thread_raise(sp_thread *t, const char *cls, const char *msg, void *obj);  /* #raise */
void       sp_Thread_pass(void);          /* Thread.pass: cooperative yield */
void       sp_Thread_stop(void);          /* Thread.stop: sleep until #wakeup */
sp_thread *sp_Thread_wakeup(sp_thread *t);  /* #wakeup */
sp_thread *sp_Thread_run(sp_thread *t);     /* #run: #wakeup, then pass */
sp_bool    sp_Thread_stop_p(sp_thread *t);  /* #stop?: dead or asleep */
void       sp_sched_sleep_forever(void);  /* a bare Kernel#sleep */
/* Kernel#sleep(seconds): park the calling thread and free its OS worker for other
   green threads; a monitor thread wakes it after the duration. Falls back to a
   plain blocking sleep only in the single-threaded build (spinel_rt.h). */
void       sp_sched_sleep(double seconds);
int        sp_sched_other_threads_live(void);
int        sp_sched_wait_child(int pid, int *status);
void       sp_sleep(sp_float s);   /* Kernel#sleep; relocated from spinel_rt.h to lib/sp_cold.c */
void       sp_sleep_forever(void);  /* a bare Kernel#sleep, until Thread#wakeup */
/* Scheduler-aware blocking I/O: park the calling green thread until `fd` is ready
   for `events` (POLLIN/POLLOUT), freeing its OS worker for other threads; the
   monitor polls the fd and wakes it. Returns 1 to retry the syscall (fd ready or
   errored), 0 to give up (shutdown). Falls back to a plain blocking poll in the
   single-threaded build / before the monitor starts. */
int        sp_sched_wait_io(int fd, short events);
/* The same over a SET of descriptors (IO.select over several handles): park
   until any entry is ready or the timeout passes (negative: none). Returns 1
   when something is ready -- the caller polls the set with a zero timeout to
   learn which -- and 0 on the timeout. A blocking poll here held the OS
   worker for the wait, and a worker in a syscall never reaches a safepoint,
   so a collection waited on it (#4528). */
int        sp_sched_wait_io_set(struct pollfd *set, int n, double timeout_s);
/* A handle is closing: drop any persistent readiness registration for `fd`
   while the descriptor still names the right thing. Safe to call for a
   descriptor that was never registered, and on a build with no event set. */
void       sp_sched_ev_forget(int fd);
/* The same park with a deadline: wake when `fd` is ready for `events` OR when
   `timeout_s` seconds have passed, whichever comes first (a negative timeout
   is no deadline, i.e. sp_sched_wait_io). Returns 1 when the fd is ready or
   errored, 0 on the deadline (or shutdown). The monitor watches the fd and the
   clock together, so a timed readiness wait -- IO#wait_readable(t),
   IO.select([io], nil, nil, t) -- costs the calling green thread, not its OS
   worker; a blocking select(2) here pins the worker for the whole timeout,
   and with every worker pinned the peer that would make the fd ready never
   gets to run. */
int        sp_sched_wait_io_timeout(int fd, short events, double timeout_s);
/* sp_sched_wait_io for a handle another thread may close meanwhile: `cancel`
   is the handle's closed flag, read under the scheduler lock before the park
   registers. A close that landed after the caller's own readiness probe is
   then answered at once (returns 1; the caller finds the handle closed),
   where the registration would otherwise go in after the close's
   sp_sched_ev_forget and never be readied. */
int        sp_sched_wait_io_unless(int fd, short events, const unsigned char *cancel);
sp_thread *sp_Thread_current(void);       /* Thread.current */
sp_bool   sp_Thread_alive(sp_thread *t); /* #alive? */
sp_bool   sp_Thread_set_report_default(sp_bool v);  /* Thread.report_on_exception= */
sp_bool   sp_Thread_get_report_default(void);        /* Thread.report_on_exception */
sp_bool   sp_Thread_set_report(sp_thread *t, sp_bool v); /* #report_on_exception= */
sp_bool   sp_Thread_get_report(sp_thread *t);            /* #report_on_exception */
sp_thread *sp_Thread_main(void);          /* Thread.main */
sp_int    sp_Thread_list_count(void);     /* Thread.list (built by the TU) */
sp_thread *sp_Thread_list_at(sp_int i);
sp_RbVal   sp_Thread_get_name(sp_thread *t);                       /* #name */
sp_RbVal   sp_Thread_set_name(sp_thread *t, sp_RbVal v);           /* #name= */
sp_RbVal   sp_Thread_status(sp_thread *t); /* #status: "run"/"sleep"/false/nil */
const char *sp_Thread_inspect(sp_thread *t); /* #inspect / #to_s */
sp_RbVal   sp_Thread_tls_get(sp_thread *t, sp_sym k);              /* Thread#[] */
sp_RbVal   sp_Thread_tls_set(sp_thread *t, sp_sym k, sp_RbVal v);  /* Thread#[]= */
sp_bool   sp_Thread_tls_key(sp_thread *t, sp_sym k);             /* Thread#key? */

/* Run any remaining runnable threads to completion. Emitted at the end of
   main() so a fire-and-forget Thread still runs its body. */
void       sp_sched_drain(void);

/* Safepoint (design 5.1). codegen emits, at loop back-edges of a threaded
   program, `if (SP_UNLIKELY(SP_SAFEPOINT_POLL())) sp_safepoint();` so a worker
   in a long-running loop periodically checks whether a stop-the-world (GC)
   wants it to park. The flag is set only in the threaded runtime while a
   collector has requested STW; at N=1 it is never set, so the poll is a single
   predicted-not-taken load. Declared unconditionally (not under SP_THREADS) so
   the emitted poll also compiles when a threaded program's generated C is built
   against the single-threaded archive (e.g. the test harness's manual cc path),
   where it is an inert no-op. sp_safepoint() parks the worker at the GC barrier
   (the real body lands with the workers + STW).

   The flag is written by the collector (and the preempt signal handler) and
   read bare by every mutator; under SP_THREADS both sides go through relaxed
   atomics so the cross-thread poll is defined behavior (and TSan-clean).
   Relaxed is enough: a mutator that misses one update just polls again next
   back-edge, and sp_safepoint() itself acquires the sched lock, which orders
   everything that matters. */
extern volatile int sp_safepoint_flag;
#ifdef SP_THREADS
#define SP_SAFEPOINT_POLL() SP_ATOMIC_LOAD(&sp_safepoint_flag, __ATOMIC_RELAXED)
#define SP_SAFEPOINT_SET(v) SP_ATOMIC_STORE(&sp_safepoint_flag, (v), __ATOMIC_RELAXED)
#else
#define SP_SAFEPOINT_POLL() (sp_safepoint_flag)
#define SP_SAFEPOINT_SET(v) (sp_safepoint_flag = (v))
#endif
void sp_safepoint(void);
/* A blocking native call (an ffi_func declared `blocking: true`) is bracketed
   by these: the worker leaves the world, so a collection raised while the
   call runs does not wait for it, and a collection in progress is waited out
   on the way back. The call must touch no Ruby object. No-ops without the
   threaded runtime or before the worker pool exists. */
#ifdef SP_THREADS
void sp_native_enter(void);
void sp_native_leave(void);
#else
static inline void sp_native_enter(void) {}
static inline void sp_native_leave(void) {}
#endif

/* Optional hook the generated TU installs so a worker parking at a safepoint
   also publishes its per-worker in-flight GC roots that live in the TU (pending
   exception objects, proc-return home values) onto its shadow stack, for the
   stop-the-world collector to mark. NULL when the program has none. */
extern void (*sp_safepoint_publish_hook)(void);

/* ---- Queue (thread-safe FIFO) ----
 * A producer/consumer hand-off. #pop on an empty queue blocks the calling green
 * thread (parking it, yielding to the scheduler) until a #push wakes it; this is
 * the coordination the lazy Thread model could not express. */
typedef struct sp_queue {
  sp_RbVal         *buf;          /* ring buffer of queued values */
  sp_int           head, len, cap;
  sp_int           max;          /* SizedQueue capacity; 0 = unbounded Queue */
  struct sp_thread *pop_waiters;  /* threads blocked in #pop on an empty queue */
  struct sp_thread *push_waiters; /* threads blocked in #push on a full SizedQueue */
  int               closed;
} sp_queue;

sp_queue  *sp_Queue_new(void);
sp_queue  *sp_SizedQueue_new(sp_int max);          /* SizedQueue.new(max) */
void       sp_Queue_push(sp_queue *q, sp_RbVal v);  /* #push / #<< / #enq (blocks when full) */
void       sp_Queue_push_nb(sp_queue *q, sp_RbVal v); /* SizedQueue#push(non_block: true) */
void       sp_Queue_push_options_check(sp_queue *q, int has_non_block, int has_timeout);
sp_bool    sp_Queue_push_timeout(sp_queue *q, sp_RbVal v, double seconds); /* false on timeout */
sp_RbVal   sp_Queue_pop(sp_queue *q);               /* #pop / #shift / #deq (blocks when empty) */
sp_RbVal   sp_Queue_pop_nb(sp_queue *q);            /* #pop(truthy): no_wait, raises ThreadError on empty */
sp_RbVal   sp_Queue_pop_timeout(sp_queue *q, double seconds); /* nil on timeout */
sp_int    sp_Queue_size(sp_queue *q);              /* #size / #length */
sp_bool   sp_Queue_empty(sp_queue *q);             /* #empty? */
sp_int    sp_Queue_max(sp_queue *q);               /* SizedQueue#max */
void       sp_Queue_close(sp_queue *q);             /* #close */
sp_bool   sp_Queue_closed(sp_queue *q);            /* #closed? */
sp_int    sp_Queue_num_waiting(sp_queue *q);       /* #num_waiting */
void       sp_Queue_clear(sp_queue *q);             /* #clear */

/* ---- Mutex (non-recursive lock; owner + wait list) ----
 * Threads are kept alive by the scheduler registry, so neither struct needs a
 * GC scan over its owner/waiter pointers. */
typedef struct sp_mutex {
  struct sp_thread *owner;     /* NULL = unlocked */
  struct sp_thread *waiters;   /* threads blocked in #lock */
  /* Monitor is a REENTRANT mutex, and that is the whole reason it exists:
     `m.synchronize { m.synchronize { } }` is the ordinary way to write a
     method that is safe to call from another synchronized method of the same
     object. A Mutex must still raise ThreadError there, so reentrancy is a
     per-object flag rather than the lock's behaviour. `depth` counts the
     re-acquisitions the owner holds beyond the first. */
  int reentrant;
  int depth;
  /* Threads parked in #lock, counted so an uncontended #unlock can see that
     there is nobody to hand off to without taking the global scheduler lock
     (#4346). Mutated only under that lock; read atomically outside it. */
  int nwaiters;
} sp_mutex;

sp_mutex  *sp_Mutex_new(void);
sp_mutex  *sp_Monitor_new(void);              /* a Mutex with reentrancy on */
const char *sp_Mutex_class_name(sp_mutex *m); /* "Monitor" or "Thread::Mutex" */
const char *sp_Queue_class_name(sp_queue *q);  /* "Thread::SizedQueue" or "Thread::Queue" */
void       sp_Mutex_lock(sp_mutex *m);
void       sp_Mutex_unlock(sp_mutex *m);
sp_RbVal   sp_Mutex_sleep(sp_mutex *m, int has_timeout, double timeout);  /* #sleep / #sleep(t) */
sp_bool   sp_Mutex_try_lock(sp_mutex *m);   /* #try_lock: true if acquired */
sp_bool   sp_Mutex_locked(sp_mutex *m);     /* #locked? */
sp_bool   sp_Mutex_owned(sp_mutex *m);      /* #owned? */

/* ---- ConditionVariable (wait/signal/broadcast over a Mutex) ---- */
typedef struct sp_condvar {
  struct sp_thread *waiters;   /* threads blocked in #wait */
} sp_condvar;

sp_condvar *sp_CondVar_new(void);
/* #wait answers what CRuby's Mutex#sleep answers: nil when a timeout ran out,
   otherwise the whole seconds slept (time(2) after less time(2) before). */
sp_RbVal    sp_CondVar_wait(sp_condvar *cv, sp_mutex *m);  /* release m, park, re-acquire m */
sp_RbVal    sp_CondVar_wait_nb(sp_condvar *cv, sp_mutex *m);  /* release m, do not park, re-acquire m */
sp_RbVal    sp_CondVar_wait_timeout(sp_condvar *cv, sp_mutex *m, double seconds); /* timed #wait */
void        sp_CondVar_signal(sp_condvar *cv);             /* #signal */
void        sp_CondVar_broadcast(sp_condvar *cv);          /* #broadcast */

#endif /* SP_SCHED_H */
