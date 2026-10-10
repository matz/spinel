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

- **N OS workers.** Worker count is `min(online cores, SPINEL_WORKERS)`.
  Set `SPINEL_WORKERS=1` to force the single-worker cooperative model, or a
  fixed number to cap parallelism. Absent the env var, Spinel autodetects the
  number of online cores.
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
  (see [FFI.md](FFI.md)).
- **Preemption.** A monitor thread timeslices CPU-bound threads (~10 ms
  quantum) so a thread that loops without yielding cannot starve its siblings.
  Preemption is taken at **safepoint polls** (loop back-edges): a thread that
  spends a long time inside a single runtime call with no poll yields only when
  that call returns. The preemption signal is `SIGURG`, overridable with
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
on whichever of an fd or the deadline comes first. A backtick, `system` and
`Process.waitpid2` in a green thread wait for the child without holding the
worker either.

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

  - **Objects never lose an ivar.** Every instance variable is a field of a
    generated C struct, fixed when the program is compiled: an object's ivar
    set cannot grow at run time, and `instance_variable_set` takes a literal
    symbol only (a name decided at run time raises NoMethodError). So two
    threads assigning two different ivars cannot lose either, and no ivar
    write can be lost to a write of a *different* ivar.
  - **A word-sized ivar never tears.** A racing read of an Integer, Float,
    String, or object-reference ivar answers a value some thread wrote --
    never one nobody wrote, and never a half-written pointer. Which write it
    sees is unordered, and a read may be stale.
  - **A multi-word ivar can tear.** `Range`, `Time`, `Complex` and `Rational`
    ivars are stored by value and copied field by field, so a racing read can
    see one field from one write and another field from another: a `Range`
    ivar written concurrently reads back with `first` and `last` from
    different assignments.
  - **Containers can crash the process.** `Array` and `Hash` have no such
    guarantee. Concurrent `<<` on one Array reaches the growth path, where a
    reallocation races with another thread's element store, and the observed
    outcomes are silently dropped elements, a heap-corruption abort, and
    SIGSEGV. A shared container needs a `Mutex`, or a `Queue`, which is
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
  of writing into whatever mapping is below, and the fault is reported as
  `spinel: fiber stack overflow: ...` naming the size and the knob before
  the process dies by the signal (#4496).
- **Interleaving is nondeterministic.** The ordering of `Thread.pass`,
  `Thread.list` membership, and the exact moment a `Thread#raise` / `#kill` is
  delivered are nondeterministic, where the single-worker model was
  deterministic.

## Environment variables

| variable | effect |
|---|---|
| `SPINEL_WORKERS` | number of OS workers; overrides the online-core autodetect (min 1). Read at the first `Thread.new`, so a program can set it itself: `ENV["SPINEL_WORKERS"] = "1"` before spawning caps its own pool, and `... = "1" unless ENV["SPINEL_WORKERS"]` declares a default the environment still overrides |
| `SPINEL_PREEMPT_SIGNAL` | the signal the monitor uses to preempt a busy worker (default `SIGURG`) |
| `SPINEL_SCHED_STATS` | `1` prints, at exit, what the scheduler's monitor did (turns, polls, readiness arms). `2` also prints every five seconds three histograms of what a green thread waited for: on a run queue after being readied, in `Mutex#lock`, and in `ConditionVariable#wait`, plus how the live threads are pinned across the workers. These are the numbers a tail latency is made of when the CPU is not the bottleneck |
| `SPINEL_GC_MINOR` | `0` turns the generational object mark off: every collection then marks the whole live heap. On by default since 2026-09-12: a minor cycle marks the young objects plus what the write barrier remembered of the old ones, and a full cycle runs on the cadence the survival ratio sets (`SPINEL_GC_FULL_INTERVAL=N` pins it). `SPINEL_GC_VERIFY_GEN=1` re-marks the whole heap after every minor and names any holder the barrier missed |
| `SPINEL_GC_THRESHOLD_KB` | per-worker collection budget; raise it to trade memory for fewer stop-the-world pauses (default 256) |
| `SPINEL_GC_THRESHOLD_OBJ_KB` | the same budget for the OBJECT heap alone, overriding the pair above |
| `SPINEL_GC_THRESHOLD_STR_KB` | the same for the STRING heap alone |
| `SPINEL_GC_OBJ_BUDGET` | the default GATES the widening on what the last collection cost. `obj` pins it off (the object heap alone, as spinel did before 2026-09-09), `walk` pins it on (everything a mark walks). `fixed` is a separate axis: it stops re-aiming the budget after each collection and holds it at its floor |
| `SPINEL_GC_STR_BUDGET` | by default the string budget also carries a share of the object old generation, gated by the same `mark share`. `str` pins that off (the string heap alone, as spinel did before the share). `fixed` does for the STRING budget what it does for the object one |
| `SPINEL_GC_SLAB` | `0` turns the slab allocator off: every object and heap string is then its own malloc, which is what ASAN needs to see a use-after-free (the slab hides one). On by default whatever the process's malloc. It used to default off under jemalloc (linked or preloaded), whose thread caches did what the slab's free lists did and beside which the free-list slab measured 8% slower; the bitmap sweep is what jemalloc's caches cannot do, and with it the slab under jemalloc answers the same requests a second on 17% less CPU and 13% less memory |
| `SPINEL_SLAB_HUGE` | `0` stops the slab asking for transparent huge pages on its arenas (`madvise(MADV_HUGEPAGE)`, effective where `transparent_hugepage` is `madvise` or `always`). On by default: an arena faults in as two 2 MB pages instead of a thousand 4 KB ones, and a list benchmark spent a third of its time in those faults |
| `SPINEL_GC_CONC` | `0` sweeps under the stop-the-world barrier instead of beside the program, which is also what the verifiers (`SPINEL_GC_VERIFY`, `SPINEL_GC_VERIFY_GEN`) and aging (`SPINEL_GC_AGE`) do on their own, since they read the heap the sweep is rewriting. `SPINEL_GC_VERIFY` also checks the slab's bitmaps against their invariants at every barrier and after every chunk's sweep |
| `SPINEL_GC_PAR_MARK` | `0` marks on the collector alone. By default the mark's drain runs on the collector and up to `SPINEL_GC_MARKERS` parked workers (default min(workers, 4)), sharing the pending objects in small chunks; the verifiers and aging mark serially on their own |
| `SPINEL_GC_SWEEPERS` | how many sweeper threads sweep the old lists (default the worker count, at most 8). `SPINEL_GC_OWNER=0` hands them the young lists too, instead of each worker sweeping its own |
| `SPINEL_GC_TRIM_SEC` | how often the container buffers glibc still holds are given back to the OS with `malloc_trim` (default 1, `0` never). A trim walks every arena, tens of milliseconds on a many-core box; it runs on its own thread beside the program, but a worker that allocates from the arena being walked waits for it, so a longer interval buys latency for memory: on a 32-core server `10` took the room page's p99 from 122 to 102 ms and its RSS from 950 MB to 1.4 GB |
| `SPINEL_GC_STR_MAJOR_KB` | the string OLD generation's own gate: how much old string it takes to make the next string sweep a MAJOR (default 1024). Only a major reclaims an old string |
| `SPINEL_GC_STR_MAJOR` | the major's policy. By default it runs on a SCHEDULE (a major every N string sweeps, N adapted from the survival ratio) with the size test demoted to a backstop, which is how the object heap has always run its full collection. `size` restores the gate that shipped before it -- the size test on its own, re-aimed to twice what the last major KEPT (what it left minus what the same sweep promoted, which is no survivor of anything yet). `fixed` pins both the cadence and the backstop where the floor put them |

The schedule became the default on the measurements below, and `size` exists
because a default change should carry its own way back.

| shape | schedule against the size gate |
|---|---|
| ONCE Campfire at a 16 MB string floor (measured by the reporter, #4407) | PSS 280 -> 145 MB, throughput 345 -> 365 req/s. Past the 64 MB control on memory, and faster |
| a 12 MB live string set under a 4 MB floor (`test/gc_str_major_interval.rb`) | old generation 67.4 -> 21.4 MB, peak RSS 135 -> 84 MB, no slower |
| 400,000 retained strings no major can free | no difference in wall time or RSS: the survival ratio stretches the interval to 16 and then to 128, so the scheduled majors cost nothing |
| `benchmark/bm_threaded_render.rb` | median RSS 736 -> 776 MB over twelve order-flipped passes a side, wall time 1.92 -> 1.93s |

The last row is the cost: five percent of one benchmark's median memory, with
no time, against halving a real application's. A program that wants the old
behaviour has `SPINEL_GC_STR_MAJOR=size`.

The reason the size gate ratcheted at all is that it was aimed at a number it
produces. A small budget promotes early, promotion is one-way until a major,
and "twice what the last major left" then set the next gate from what that
early promotion inflated. A cadence cannot do that, and a survival RATIO is
scale-free, so neither can the adaptation. The backstop is now aimed at what
the major KEPT rather than at what it left: on a server, what a sweep promotes
is mostly the strings the requests in flight held at that moment, dead a few
milliseconds later, and a gate of twice kept-plus-promoted held three sweeps of
them (an old generation of 290 MB against 20 MB kept on Campfire, and the
young trigger, which retunes on the whole string heap, following it up).
Aimed at kept, the next sweep whose promotion outgrows the live set is a
major, and the old generation stays within one sweep's promotion of what is
live: peak RSS 1,024 -> 711 MB and PSS 707 -> 629 MB on the room page. What
that costs is the collections the inflated trigger had been skipping, about
3% of throughput at 64 connections on 32 cores.

The three `_KB` variables set where a budget STARTS; the collector re-aims it
from what the collection found. That is right for running a program and wrong
for asking what the re-aiming is responsible for, which is why `fixed` exists.
With it, the budget is whatever the floor says and stays there, so two builds
of one program differ by the change under test and not by two different pacing
histories.

It is a diagnostic and not a policy. Fixing the budget per worker makes the
aggregate scale with the worker count, which is exactly what the adaptive
budget is shaped to avoid: on a workload here the adaptive aggregate trigger
is 224 MB at one worker and 228 MB at eight, while a budget pinned at 64 MB
per worker is 128 MB at one and 576 MB at eight, and the resident set follows
it from 253 MB to 1.4 GB.


The two per-heap variables exist to answer a question the pair cannot. The
mark walks both live sets, and only one heap's trigger decides when it runs.
Raising both together speeds the program up without saying which of them was
pacing it; raising one says. Point `SPINEL_GC_STATS=1` at the result and the
`trigger` field reports the two separately:

```
[gc] 19 collections ... trigger 512.0 MB obj + 0.25 MB str/worker
```

The object figure is the pool-wide budget (the base times the worker count,
for the reason in the note below); the string figure is per worker, which is
why the string one is not multiplied.

The object budget is what may be allocated before the next collection, and
what pays for it is what that collection costs -- and a collection marks BOTH
heaps. Pricing the budget off the object live set alone therefore prices it
off the wrong quantity: a program can grow its string set without the
collection rate noticing, and the mark per unit of work climbs. Since
2026-09-09 the budget is priced off both, and `SPINEL_GC_OBJ_BUDGET=obj`
restores the old behaviour.

Measured on a threaded web application at two concurrencies: 26-44% more
requests per second, at within 5-10% of the memory a fixed
`SPINEL_GC_THRESHOLD_OBJ_KB` floor costs for the same work. The reason to
prefer it over that floor is that it tracks: it settles around 70-78 MB where
the floor would pin 128, and around 227-253 MB where the floor would be too
small. Any fixed number is wrong at one end of a concurrency range.

Since 2026-09-10 it also asks whether the mark is what you are paying for,
which the first version of this did not: it widened the budget by the mark set
either way, so a program holding a large live string set while collecting
cheaply paid memory and got nothing back.

The budget now widens by `alpha` times the string live set, where `alpha` is
the share of a collection the MARK is. `SPINEL_GC_STATS=1` reports it as
`mark share` on the `[gc]` line, so the policy a program is getting is
readable rather than inferred. A program whose collections are nearly all mark
gets what `walk` pins by hand; one whose collections are nearly all sweep gets
what `obj` pins; the two sit at opposite ends of one number instead of needing
different settings.

Two things about how alpha is computed are worth knowing, because both were
arrived at the hard way.

It is counted in OBJECTS MARKED and SLOTS SWEPT, not bytes. A cost ratio taken
per byte does not carry between programs: a cache of large strings and a churn
of small arrays hold the same megabytes with slot counts fifty times apart.
The sweep's cost is per slot and the mark's is per live object, so counted,
the coefficients belong to the collector rather than to a program's allocation
sizes.

And the coefficient relating them is an ORDER rather than a measurement.
Measured on one machine, a mark is 360-560 ns an object and a slot sweep is
about 8 ns serially against about 120 ns across eight workers, where the
parked-worker coordination and the string sweep fold in. Writing those numbers
down would pin one machine's ratio into the collector. Written as the order
they sit at -- about 64 serially, about 4 in parallel -- the answer barely
moves: on the pair of programs that motivated the gate the measured
coefficients give 0.012 and 0.57, the orders give 0.016 and 0.55. The decision
was never close enough for the precision to be worth claiming.

The STRING budget is priced the same way from the other side. What it pays
for is a whole collection too, and a program that keeps OBJECTS and throws its
strings away -- a parser appending records to an Array -- has a string live
set near zero however large its object heap grows. Priced off the string heap
alone, its budget stayed at the 256 KB floor while every collection that
bought cost more: a minor cycle re-reads each old container written since the
last one from end to end, and one string sweep in eight is a major, which
makes the cycle full. The collections grew with the work and each of them with
the heap.

So the string budget carries a share of the object OLD generation: one eighth
of it, times the same `alpha`. `SPINEL_GC_STATS=1` reports what that put into
the trigger as `old obj in str trigger` on the `[gc]` line.
`SPINEL_GC_STR_BUDGET=str` pins the share off and `walk` pins it on, the two
ends `SPINEL_GC_OBJ_BUDGET` has.

Wall time and peak resident set on one laptop core, the same binary with
`str` and without:

| program | `str` | default |
|---|---|---|
| parse 1M / 2M / 4M lines into records appended to one Array | 1.9 / 8.5 / 31.8 s, 90 / 176 / 350 MB | 0.24 / 0.45 / 0.94 s, 119 / 240 / 474 MB |
| the same into a Hash, 1M | 15.1 s, 175 MB | 0.99 s, 199 MB |
| `benchmark/bm_csv_build.rb` | 1.11 s, 95 MB | 0.24 s, 130 MB |
| 1M old records in an Array, swap two a turn and drop a String, 6M turns | 24.5 s, 74 MB | 0.57 s, 92 MB |
| the same heap never written, drop an object and a String a turn | 1.03 s, 75 MB | 0.20 s, 116 MB |

The first four are the shape it is for, and they stop being quadratic: the
collections no longer grow with the work (745, 1,494 and 2,996 on the first
row against 56, 63 and 70). The last row is the shape it is not for -- nothing
is re-read there and only the scheduled full mark was being paid -- and shows
the price next to the gain.

What it costs is memory: dead strings may reach a quarter of the object old
generation between collections, and with fewer collections the object heap
fills to its own budget where the string trigger used to fire first. A heap
that no collection has to re-read pays that for little: the last row of the
table, and `test/gc_str_budget_still.rb`, which peaks at 56 MB instead of 26
for the same wall time -- the gate declines the share there except on the
cycle after a full mark, and the scheduled string major makes one cycle in
eight full. One eighth is where the measurements put the knee. From the whole old generation down to an eighth of it the Array rows do
not move and their resident set falls by 27 to 40%; past an eighth their time
climbs back. The whole of it also made `benchmark/bm_str_readonly_param.rb`
hold 98 MB instead of 34 for no time at all, which an eighth does not. The
Hash row is the one that would take more -- a Hash costs more to re-read, and
it runs 1.4 times faster with the whole old generation in its budget. And an
eighth is the order of the major's cadence, which is how often a string sweep
buys the full mark.

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
