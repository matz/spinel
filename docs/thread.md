# `Thread` in Spinel

Spinel runs Ruby `Thread`s as **true parallelism with no GVL**. A threaded
program is scheduled onto `N` OS workers that run green threads over a
stop-the-world garbage collector, so CPU-bound threads make progress on
separate cores at the same time. This is the JRuby / TruffleRuby model, not
CRuby's: individual operations are **not** made atomic by a global lock.

This document describes what you can rely on when you use threads, which of the
`Thread` API is supported, and the semantics that differ from CRuby. The
implementation (the M:N scheduler, per-worker run queues, work stealing, the
preemption monitor) is a separate concern and lives in
[internals/thread-mn-design.md](internals/thread-mn-design.md) -- nothing there
is a user guarantee.

## The execution model

- **N OS workers.** `SPINEL_WORKERS=N` is the number of OS threads that run
  Ruby code, the main thread among them: N-1 helper workers run the other
  threads beside main. Set `SPINEL_WORKERS=1` to force the single-worker
  cooperative model: main runs every thread itself, giving each runnable one
  a timeslice whenever its own code has run a slice, as in CRuby. One OS
  thread runs them all, so a call that blocks in the OS rather than in the
  scheduler (`flock` waiting for a lock, a blocking extension call) stops
  every thread until it returns; I/O, sleeps and Queue / Mutex waits park in
  the scheduler and do not. A fixed N above 1 caps parallelism. Absent the env var, Spinel autodetects the
  number of online cores and runs a helper per core beside main.
  Main runs no thread it did not start on itself: the threads go to the
  helpers. So a program whose main thread only starts threads and joins
  them (a server's main, typically) has N-1 OS threads of work under
  `SPINEL_WORKERS=N`, and `SPINEL_WORKERS=2` runs every thread on one
  helper. Give such a program `SPINEL_WORKERS=N+1` for N threads of work,
  or leave the variable unset.
- **Real parallelism, no GVL.** Two threads run Ruby code on two cores
  simultaneously. There is no implicit per-object lock, so unsynchronized
  shared mutation is a data race (see [By design](#by-design) below).
- **Green threads over a stop-the-world GC.** A `Thread` is a green thread; the
  workers multiplex many green threads onto few OS threads. A garbage
  collection stops all workers at a safepoint, so allocation stays correct
  under parallelism without per-object write barriers. A worker with nothing
  to run, waiting in the scheduler, is *out of the world*: its roots are
  published when it goes idle, a collection counts it as parked without
  waking it, and it parks for real only if it wakes while one is running.
  The same is true of a worker inside an `ffi_func` declared `blocking: true`
  (see [FFI.md](FFI.md)). The green threads that share that worker wait
  while the call runs C: a thread stays for life on the worker it first ran
  on, and no other worker runs it. A callback from the call that sleeps or
  waits on I/O parks its thread and lets them run meanwhile. A call declared
  `offload: true` instead runs on an offload thread, and the caller waits as
  for I/O, leaving its worker to the others (Spinel's own; the C function runs
  on another OS thread).
- **Preemption.** A monitor thread timeslices CPU-bound threads (~10 ms
  quantum, `RUBY_THREAD_TIMESLICE`) so a thread that loops without yielding
  cannot starve its siblings.
  Preemption is taken at **safepoint polls** (loop back-edges): a thread that
  spends a long time inside a single runtime call with no poll yields only when
  that call returns. A loop nested in another that counts `i` up to
  `a.length` over an array it stores no element of, and runs no other code,
  polls at the enclosing loop's back-edge instead of its own, so it yields up
  to one pass over that array late, as a runtime call over the array would.
  The preemption signal is `SIGURG`, overridable with
  `SPINEL_PREEMPT_SIGNAL`.
- **The single-threaded archive is unchanged.** A program that never uses
  `Thread` compiles and runs byte-identically to before; none of the scheduler
  machinery is linked in.

## Supported API

### `Thread`

| method | notes |
|---|---|
| `Thread.new(*args) { \|*args\| ... }` | spawn; args become the block parameters |
| `#join` | block until finished, re-raising an unhandled exception in the caller |
| `#value` | `#join` plus the block's result |
| `#kill` / `#exit` / `#terminate` | terminate the thread |
| `#raise(...)` | raise an exception in the thread |
| `Thread.pass` | cooperative yield |
| `Thread.current` / `Thread.main` | the running / initial thread |
| `Thread.list` | live threads |
| `#alive?` / `#status` | `"run"` / `"sleep"` / `false` / `nil` |
| `#name` / `#name=` | a string or nil |
| `Thread#[]` / `#[]=` / `#key?` | fiber-local / thread-local storage by symbol |
| `Thread.report_on_exception` / `=` and the per-thread accessors | unhandled-exception reporting |

`Kernel#sleep` and blocking I/O are **scheduler-aware**: a sleeping or
I/O-blocked thread frees its OS worker for other green threads instead of
holding it idle, and the monitor wakes it when the deadline passes or the fd
becomes ready. Every read entry point on a socket, a pipe or a terminal
(`#gets`, `#read`, `#readpartial`, `#sysread`, `#getc`, `#getbyte`,
`#readbyte`, `#eof?`, `#each_line` and the rest) parks this way, and so does
a readiness wait -- `IO#wait_readable(t)`, `IO#wait_writable(t)`,
`IO.select` over one IO or several, with or without a timeout -- which wakes
on whichever of an fd or the deadline comes first. An
`OpenSSL::SSL::SSLSocket`'s blocking calls (`connect`, `sysread`,
`syswrite`, and the buffered reads and writes over them) wait the same way,
on the descriptor between non-blocking record-layer steps. A backtick,
`system` and `Process.waitpid2` in a green thread wait for the child without
holding the worker either, and so do the waits with flags (`Process.wait2(pid,
Process::WUNTRACED)`); `Process::WNOHANG` asks once and answers nil for a child
still running, with no wait at all.

Why this matters beyond fairness: a collection stops the world at
safepoints, and a worker sitting in a syscall never reaches one, so a thread
that blocked its worker in the kernel would hold up every other thread's
next allocation for as long as the syscall lasted. A `blocking: true` FFI
call is the one place a worker deliberately leaves for the kernel, and it
announces that on the way out so the collector does not wait for it.

### Synchronization primitives

Real, blocking primitives -- not busy-waits:

| type | methods |
|---|---|
| `Mutex` | `#lock` / `#unlock` / `#try_lock` / `#locked?` / `#owned?` (non-recursive) |
| `Queue` | `#push` / `#<<` / `#pop` / `#size` / `#empty?` / `#close` / `#closed?` / `#clear` (`#pop(timeout:)` returns `nil` when its deadline expires) |
| `SizedQueue` | `Queue` plus `#max`; `#push(timeout:)` returns `nil` if the queue stays full until its deadline |
| `ConditionVariable` | `#wait(mutex[, timeout])` / `#signal` / `#broadcast`; nil timeout waits indefinitely |

## By design

These are deliberate consequences of real parallelism, listed in
[limitations.md](limitations.md#by-design-deliberate-choices):

- **Data races are observable.** Two threads mutating the same
  `Array`/`Hash`/object without a `Mutex` race, similar to `Array`/`Hash` in
  JRuby and `Array` in TruffleRuby. CRuby's GVL makes individual operations
  appear atomic; Spinel does not, and adds no implicit per-object locking.
  Correctness across threads is the program's responsibility via
  `Mutex` / `Queue` / `ConditionVariable`.

  What "race" means here is worth stating rather than leaving as *undefined*,
  because the kinds of state differ (#4166):

  - **Objects never lose an ivar.** An instance variable the program names
    is a field of a generated C struct, fixed when the program is compiled.
    So two threads assigning two different ivars cannot lose either, and no
    ivar write can be lost to a write of a *different* ivar. An instance
    variable whose name is decided at run time (`instance_variable_set`
    with a computed name) lives in a table of the object beside its
    fields; that table is read and written under a lock of the runtime,
    so two threads setting two such names each keep theirs as well, and
    `instance_variables` lists only names that were set.
  - **A word-sized ivar never tears.** A racing read of an Integer, Float,
    String, or object-reference ivar answers a value some thread wrote --
    never one nobody wrote, and never a half-written pointer. Which write it
    sees is unordered, and a read may be stale.
  - **A multi-word ivar can tear.** `Range`, `Time`, `Complex` and `Rational`
    ivars are stored by value and copied field by field, so a racing read can
    see one field from one write and another field from another: a `Range`
    ivar written concurrently reads back with `first` and `last` from
    different assignments.
  - **Containers can crash the process.** `Array`, `Hash` and `IO::Buffer`
    have no such guarantee. Concurrent `<<` on one Array reaches the growth
    path, where a reallocation races with another thread's element store, and
    the observed outcomes are silently dropped elements, a heap-corruption
    abort, and SIGSEGV. An `IO::Buffer` freed, resized or transferred while
    another thread reads it is the same race: the reader can be left holding
    memory already given back. A loop may keep an array's or a buffer's data
    across its iterations and see another thread's change only at its next
    safepoint. A shared container needs a `Mutex`, or a `Queue`, which is
    itself thread-safe.
- **A green thread's C stack is fixed.** Every thread, Fiber and generator
  Enumerator body runs on its own C stack, 256 KB by default (half of
  CRuby's machine stack for a fiber); the process stack is only the main
  thread's. The mapping is address space until a page is touched, so a fiber
  costs the depth it actually reaches, not the size. `SPINEL_FIBER_STACK=<bytes>`
  (`K`/`M` suffixes) in the environment sets the size for every fiber a run
  creates, and a program built at `-O 0` or `-O 1` (or `--debug`) asks for
  1 MB by itself, since an unoptimised build's frames are many times
  `-O2`'s; the build-time default is `SP_FIBER_STACK_SIZE` in lib/sp_fiber.h.
  Below the stack sits a 256 KB guard (`SP_FIBER_GUARD_SIZE`, never
  touched), so a frame that steps past the end faults in the guard instead
  of writing into whatever mapping is below (#4496). The fault raises
  `SystemStackError` in that fiber, as the main thread's overflow does: a
  `rescue` in the body takes it, and one nothing rescues ends the thread
  (raised again at `#join` / `#value`), the Fiber (at `#resume`) or the
  Enumerator (at `#next`). A depth CRuby reaches but 256 KB does not is
  `SPINEL_FIBER_STACK`'s to give.
- **Interleaving is nondeterministic.** The ordering of `Thread.pass`,
  `Thread.list` membership, and the exact moment a `Thread#raise` / `#kill` is
  delivered are nondeterministic, where the single-worker model was
  deterministic.

## Environment variables

| variable | effect |
|---|---|
| `SPINEL_WORKERS` | number of OS workers; overrides the online-core autodetect (min 1). Read at the first `Thread.new`, so a program can set it itself: `ENV["SPINEL_WORKERS"] = "1"` before spawning caps its own pool, and `... = "1" unless ENV["SPINEL_WORKERS"]` declares a default the environment still overrides |
| `SPINEL_PREEMPT_SIGNAL` | the signal the monitor uses to preempt a busy worker (default `SIGURG`) |
| `RUBY_THREAD_TIMESLICE` | the timeslice in milliseconds, as CRuby reads it (default 10; CRuby's is 100). A thread woken onto a worker that runs more CPU-bound threads waits behind their timeslices: a shorter one lets it run sooner, but costs those threads throughput whether anything wakes or not. Read at the first `Thread.new` |
| `SPINEL_WAKE_PREEMPT` | `1` puts a woken thread at the front of its worker's queue, behind any woken before it, and the thread running there yields to it at its next safepoint, instead of the woken one waiting out the timeslices ahead of it. Off by default: it costs nothing where nothing wakes, but where wakes are frequent the threads it preempts lose throughput. Read at the first `Thread.new` |
| `SPINEL_FIBER_STACK_CACHE` | how many freed fiber stacks are kept for reuse (default 32; `0` maps and unmaps one per fiber, as before). A thread's stack goes back when its body returns, and the next thread or fiber of the same size takes it with its guard still in place, so `Thread.new` does not map a stack at all in a steady state. A kept stack keeps the pages its last fiber touched, so the cache holds at most that many stacks' worth of memory, and less than leaving every finished thread's stack mapped until the next collection, as before. Off under AddressSanitizer |
| `SPINEL_SCHED_STATS` | `1` prints, at exit, what the scheduler's monitor did (turns, polls, readiness arms). `2` also prints every five seconds three histograms of what a green thread waited for: on a run queue after being readied, in `Mutex#lock`, and in `ConditionVariable#wait`, plus how the live threads are pinned across the workers. These are the numbers a tail latency is made of when the CPU is not the bottleneck |
| `SPINEL_GC_MINOR` | `0` turns the generational object mark off: every collection then marks the whole live heap. On by default since 2026-09-12: a minor cycle marks the young objects plus what the write barrier remembered of the old ones, and a full cycle runs on the cadence the survival ratio sets (`SPINEL_GC_FULL_INTERVAL=N` pins it). `SPINEL_GC_VERIFY_GEN=1` re-marks the whole heap after every minor and names any holder the barrier missed |
| `SPINEL_GC_THRESHOLD_KB` | per-worker collection budget; raise it to trade memory for fewer stop-the-world pauses (default 256) |
| `SPINEL_GC_THRESHOLD_OBJ_KB` | overrides the collection floor set by `SPINEL_GC_THRESHOLD_KB` |
| `SPINEL_GC_OBJ_BUDGET` | `fixed` holds the collection threshold at its floor. `obj` and `walk` select the endpoints for the reported mark share. All collected objects, including Strings, use the same budget |
| `SPINEL_GC_SLAB` | `0` turns the slab allocator off: every object and heap string is then its own malloc, which is what ASAN needs to see a use-after-free (the slab hides one). On by default whatever the process's malloc. It used to default off under jemalloc (linked or preloaded), whose thread caches did what the slab's free lists did and beside which the free-list slab measured 8% slower; the bitmap sweep is what jemalloc's caches cannot do, and with it the slab under jemalloc answers the same requests a second on 17% less CPU and 13% less memory |
| `SPINEL_GC_SLAB_MB` | wasm32-wasi only: the slab's size in MB, rounded down to 4 (default 64; below 4 the slab is off). See [wasm.md](wasm.md) |
| `SPINEL_SLAB_HUGE` | `0` stops the slab asking for transparent huge pages on its arenas (`madvise(MADV_HUGEPAGE)`, effective where `transparent_hugepage` is `madvise` or `always`). On by default: an arena faults in as two 2 MB pages instead of a thousand 4 KB ones, and a list benchmark spent a third of its time in those faults |
| `SPINEL_GC_CONC` | `0` sweeps under the stop-the-world barrier instead of beside the program, which is also what the verifiers (`SPINEL_GC_VERIFY`, `SPINEL_GC_VERIFY_GEN`) and aging (`SPINEL_GC_AGE`) do on their own, since they read the heap the sweep is rewriting. `SPINEL_GC_VERIFY` also checks the slab's bitmaps against their invariants at every barrier and after every chunk's sweep |
| `SPINEL_GC_PAR_MARK` | `0` marks on the collector alone. By default the mark's drain runs on the collector and up to `SPINEL_GC_MARKERS` parked workers (default min(workers, 4)), sharing the pending objects in small chunks; the verifiers and aging mark serially on their own |
| `SPINEL_GC_SWEEPERS` | how many sweeper threads sweep the old lists (default the worker count, at most 8, and none where the process may use a single CPU: the collector then sweeps under the barrier, as with `SPINEL_GC_CONC=0`). `SPINEL_GC_OWNER=0` hands them the young lists too, instead of each worker sweeping its own |
| `SPINEL_GC_TRIM_SEC` | how often the container buffers glibc still holds are given back to the OS with `malloc_trim` (default 1, `0` never). A trim walks every arena, tens of milliseconds on a many-core box; it runs on its own thread beside the program, but a worker that allocates from the arena being walked waits for it, so a longer interval buys latency for memory: on a 32-core server `10` took the room page's p99 from 122 to 102 ms and its RSS from 950 MB to 1.4 GB |

The configured worker limit scales the floor. `SPINEL_WORKERS` sets that
limit, and the pool starts with one worker and adds helpers on demand.
The collector retunes the threshold after each collection in adaptive
mode. GC stress bypasses worker scaling and retuning. With
`SPINEL_GC_STATS=1`, the `trigger` field reports the pool-wide budget in
MB. See [The collector](internals/gc.md) for the counters and phases.

### A note on allocation-heavy threads

Collection stops every worker, so a program that allocates hard pays for
each pause N times over. The budget therefore scales with the worker
count: eight workers collect at eight times the heap size one worker
does, which costs a few megabytes of retained garbage and removes most
of the multi-worker penalty. On one allocation-bound benchmark that took
eight workers from 1.7x **slower** than a single worker to roughly par.

CPU-bound threads scale nearly linearly (measured 8.25x on eight
workers). Allocation-bound ones scale too, though not as steeply.

The sweep does not stop the world. Once the mark is done the collector
closes the allocation epoch (everything allocated from then on is in the
other of two young bitmaps; see below), detaches the lists of the objects
too large for the slab, and resumes the program. Each worker then sweeps
its own chunks and lists before it runs any program, so the memory it
frees is the memory it allocates from next, still warm in its cache; the
lists of the large old objects go to a pool of sweeper threads that run
beside the program. What the sweep produces that the program must not
see mid-way (the budget for the next cycle) is applied at the next stop;
an explicit `GC.start` waits for the sweep before it returns. The pause
is the mark alone, and the mark's drain runs on the collector plus a few
of the parked workers, which take pending objects from it in small chunks
(the roots are walked by the collector alone). On a 32-core box running a
Rails-shaped server the stop-the-world sweep was 30% of the wall clock;
without it, and with the drain shared, the same server answers 20% more
requests a second with a p99 20-50% lower, and the pause is under a
millisecond.

A process that may use a single CPU (its affinity mask, or a container's
CPU quota) starts no sweeper thread and sweeps under the barrier instead.
A sweeper there cannot run beside the program, only in its place, so each
collection paid the wake-up and the context switches there and back for a
sweep the collector does in the same time. On a program that allocates
hard on one CPU that was 6 to 11% of the wall clock and nearly all of its
context switches; with two CPUs or more nothing changes.
`SPINEL_GC_SWEEPERS=N` starts the sweepers all the same.

The sweep does not touch the dead either. A slab block's generation, its
mark and whether its death runs a finalizer are bits in its chunk's
bitmaps (seven per chunk, kept in the arena beside the chunk headers), and
the mark promotes what it reaches by setting the old bit there. So after
the mark, the young bitmap of the epoch that just closed holds exactly the
dead and the promoted, and a chunk is swept in a few words: what is young
and unmarked is dead, what is old and unmarked is dead at a full cycle,
the dead with a finalizer bit run their finalizer or recycler, and the
epoch's word is cleared whole. The list sweep it replaced walked every
dead object's header to unlink it and to thread a free list through it,
at memory latency, and on the same server that walk was a fifth of the
process: without it the process answers 13% more requests a second on 17%
less CPU. `SPINEL_GC_AGE` (a survivor stays young until its second
survival) applies to the lists only; with the slab on it is off, since
the remembered-set repair it needs walks the old list for what a cycle
promoted, and a slab promotion is a bit no list carries.

Since the sweep leaves the dead alone, the object pools (the per-thread
free lists of dead containers a recycler kept for the next allocation) are
bypassed with the slab on: a pooled header is one the sweep has to touch
dead to run its recycler, where an unpooled one dies in its chunk's bitmap
untouched, and the slab's own allocation, a bit found and claimed in one
word the last search cached, costs what a pop did. The general Array keeps
its first eight elements inside the object for the same reason: an array
that never outgrows them has no payload, so no finalizer, so nothing about
its death is anyone's work. On glibc the runtime also raises the trim
threshold (32 MB, with a 1 MB top pad), since the payloads that do not fit
the slab were handed back to the kernel and faulted in again on every
cycle.

What the pools had over the first bitmap allocator was the cost of one
allocation: a pop was a dozen instructions, a bit found and claimed and
zeroed was a hundred, and a tree benchmark that allocates fifteen million
nodes felt every one of them. The allocation is a bump now. A worker
claims a whole run of consecutive free slots of the word it is working
from, in the bitmap, in one write, and hands the slots out by advancing a
pointer; the collector unclaims every worker's remainder under the barrier
before it reads any young bit. The front of the allocation has no call on
its path, so it keeps no frame, and its zeroing is the class size in
16-byte stores. An arena is asked for huge pages where the kernel offers
them (transparent_hugepage=madvise), which took a list benchmark's page
faults from a third of its time to nothing. With the three, the tree and
list benchmarks that lived in the pools are back at or under the free
lists' times, and so is everything else.

Two more things the same measurements turned up. A worker keeps a reserve
of fully free chunks across a full cycle so that it does not hand them to
the kernel and fault them in again the next cycle; the reserve was one
count shared by all the size classes, spent by the first few, so the rest
carved fresh chunks every cycle (37,000 a second on a server, each an
madvise and four faults). It is per class now, sized from what the class
took in the cycle. And on a full cycle the remembered set's dirty bits are
cleared through the set's own array, as the concurrent form always did,
instead of by a walk of every old object.
