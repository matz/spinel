# M:N thread scheduler — design notes

> A **working document**, not a user guarantee. The user-visible contract lives
> in [../thread.md](../thread.md); everything here is *how* it is built and may
> change without notice. The authoritative source is `lib/sp_sched.{c,h}` (the
> header carries the per-function rationale); this file is the map.

Spinel schedules Ruby `Thread`s as green threads over `N` OS workers with real
parallelism and no GVL. The design grew in phases; the section numbers below
(§5, design 3.x, design 8, Appendix B) are the ones the commit history and the
source comments refer to.

## Green threads on the fiber substrate

A Ruby `Thread` is a green thread (`sp_thread`) wrapping an `sp_Fiber` that the
codegen builds **exactly like a `Fiber.new` block** — the capture / self / cell
machinery is shared, and the block's result lands in `fiber->yielded_value`.
This is why `Thread` and `Fiber` share `emit_fiber_new`: a thread is a fiber
plus a scheduling record.

`sp_thread` (in `sp_sched.h`) carries the fiber, the spawn arg, the return
value / unhandled-exception triple, the run state
(`RUNNABLE`/`RUNNING`/`BLOCKED`/`DEAD`), the run-queue and wait-list links, the
joiner list, thread-local storage, and the wait-list/timer fields. Threads are
kept alive by the scheduler's live-thread registry (`all_next`/`all_prev`), so
the `Mutex`/`Queue`/`ConditionVariable` structs never need to GC-scan their
owner/waiter pointers — the registry already roots them.

## Phase 0: cooperative core (N = 1)

The starting point (still the `SPINEL_WORKERS=1` model). The main program runs
on the **root fiber** and *is* the scheduler hub: it executes top-level code
directly and, whenever it blocks (`#join` / `#value` / `Thread.pass`) or at
program exit (`sp_sched_drain`), it pumps a FIFO run queue of runnable green
threads. A spawned thread yields by transferring back to the root fiber, where
the pump loop — or the fiber trampoline, on termination — resumes it. With a
single worker there is no concurrent heap mutation, so the GC and allocator are
untouched.

## Workers and run queues (design 3.x, Appendix B)

- **Helper OS workers.** `sp_worker_count()` = `min(online cores,
  SPINEL_WORKERS)`, clamped to `SP_MAX_WORKERS` (256). Each worker runs a
  root-fiber pump loop; worker 0 is the main thread, whose native stack is its
  root fiber.
- **Per-worker local run queues + a global queue (design 3.1).** A spawned or
  woken thread lands on the **global** run queue `g_grq`; each worker also has a
  **local** queue `g_lrq[wid]` for rerun locality. A worker picks
  **local-first, then global, then steals** one runnable thread from a busier
  worker. Work stealing keeps all workers fed without a single hot queue.
- **One lock.** A single mutex guards all scheduler/sync metadata (the run
  queues, the live-thread registry, wait lists). `g_runnable` counts threads
  sitting in any queue.
- **Off-cpu handshake.** `off_cpu` / `wake_pending` prevent a waker from
  enqueueing a thread onto a second worker while it is still mid-context-switch
  off its first worker's stack.

## Stop-the-world GC

Parallel workers mutate the heap, so collection is **stop-the-world at a
safepoint**. The codegen emits, at loop back-edges of a threaded program,
`if (SP_UNLIKELY(sp_safepoint_flag)) sp_safepoint();` (design 5.1). A loop
the header cache proves (`i = 0; while i < a.length ... i += 1`, a body that
runs no code) and that stores no element polls at the back-edge of the loop
of its function that encloses it, not at its own: it ends within one pass over
an array nothing in it can grow, which bounds how long a collection waits for
it as a runtime call over that array does. A loop over such a body that polls
(the proved one that stores, or one counted to an Integer bound the body
leaves as it is) polls once per run of up to 1024 passes, ahead of its test:
the run ends at the length or bound the test just read, or where the index
reaches in 1024 steps, so a collection waits at most that many of its cheap
passes, and the C compiler can vectorize the run. The flag is
set only in the threaded runtime while a collector has requested STW; at N=1 it
is never set, so the poll is a single predicted-not-taken load. It is declared
unconditionally (not under `SP_THREADS`) so a threaded program's generated C
still compiles against the single-threaded archive (e.g. the test harness's
manual `cc` path), where the poll is an inert no-op.

`sp_safepoint()` parks the worker at the GC barrier. A worker parking there also
runs `sp_safepoint_publish_hook` (when the TU installs one) to publish its
per-worker in-flight roots that live in the generated TU — pending exception
objects and proc-return home values — onto its shadow stack so the collector
marks them. The GC mark hook also roots every worker's root fiber and every live
green thread from the registry.

## The monitor thread (sysmon, §5)

A dedicated monitor thread ("sysmon"), off the workers' backs, has three jobs:

1. **Fire timers.** `Kernel#sleep`, timed I/O waits, and timed synchronization
   waits append handles to a staging buffer. The monitor batches them into a
   `CLOCK_MONOTONIC` min-heap, expires its root when due, and periodically
   compacts canceled handles. (In the single-threaded build, sleep falls back
   to a plain blocking sleep.)
2. **Timeslice preemption.** The monitor watches how long each worker has run
   its current green thread (`g_wslot[]`, re-checked every `SP_PREEMPT_TICK` =
   5 ms). Past the ~10 ms quantum it sets the thread's `preempt_request` and
   signals the worker with `g_preempt_sig` (`SIGURG`, overridable via
   `SPINEL_PREEMPT_SIGNAL`). The thread yields at its next safepoint poll —
   cooperative preemption, so a region with no poll is not yet interruptible.
3. **Scheduler-aware I/O (design 8).** `sp_sched_wait_io(fd, events)` parks a
   green thread on an fd (POLLIN/POLLOUT), freeing its worker; the monitor
   rebuilds a `poll()` set (`g_pfds`) from the I/O-waiter list each tick and
   wakes the thread when the fd is ready. `sp_sched_wait_io_timeout(fd, events,
   seconds)` uses the timer heap to wake with no revents when its deadline
   passes, which is how `IO#wait_readable(t)` and a one-io `IO.select` wait
   without pinning a worker. A self-pipe (`g_sysmon_pipe`) wakes the
   monitor out of `poll()` when the wait set changes.

The monitor idles on `g_sysmon_cv` when there is nothing to watch and is woken
via `sp_sysmon_wake()` (condvar signal, or a write to the self-pipe if it is
blocked in `poll()`).

## Synchronization primitives

All three park the calling green thread (yielding to the scheduler) rather than
busy-waiting, and hold their waiters on intrusive wait lists:

- **`Mutex`** — non-recursive: an `owner` pointer (NULL = unlocked) and a
  `waiters` list. `#unlock` hands off to the next waiter.
- **`Queue` / `SizedQueue`** — a ring buffer with `pop_waiters` (blocked on
  empty) and `push_waiters` (blocked on a full `SizedQueue`, `max > 0`). This is
  the producer/consumer hand-off the lazy Phase-0 model could not express.
- **`ConditionVariable`** — `#wait` releases the mutex, parks on the condvar's
  `waiters`, and re-acquires the mutex on wake; `#signal` / `#broadcast` unpark.
  A timeout schedules a deadline in the scheduler's staged timer min-heap and
  follows the same wake-and-reacquire path when it expires. Timed parking uses
  the same wait-list mechanism as other blocking operations, optionally
  releasing a mutex, and reports whether the deadline or another wake caused
  the thread to resume.

## Status vs. the source

Everything above is implemented on `master`. The near-term rough edges tracked
in [../limitations.md](../limitations.md) (the `Thread` row):

- Preemption is **cooperative at safepoints**, not fully async: a long region
  inside a single runtime call with no poll yields only when the call returns.
  Fully signal-interrupted preemption of such regions is future work.
- Per-worker allocation (TLAB) is not yet in; allocation goes through the shared
  heap under the scheduler lock's coordination.

When updating this file, keep the `Thread` row in `../limitations.md` and the
summary in `../thread.md` in sync — those are the user-facing surfaces.
