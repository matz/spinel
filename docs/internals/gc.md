# The collector

Spinel's garbage collector is **mark and sweep, non-moving, and precise**:
every root is registered explicitly, so no stack or register is ever scanned
conservatively and no pointer is ever guessed at.

Strings use the object heap. A Ruby String is an `sp_String *`, with
an `sp_gc_hdr` before the handle. Code borrowing its bytes can register
a byte root. The marker byte and the `sp_str_hdr` before the payload
let the collector follow that view back to its owner. See
[Marker bytes](#marker-bytes).

## Roots

`SP_GC_ROOT(v)` pushes `&v` onto a per-worker array of `void **` and relies on
GCC/clang's `cleanup` attribute to pop it when the declaring scope ends. There
is no manual unroot, and no way to leak a root by taking an early return.

A generated adapter that captures an existing object in a Proc roots that
object across Proc allocation. The Hash default-Proc adapter does this for
its source Hash; the Proc owns that reference only after allocation finishes.

The low two bits of the stored address are a tag:

| Tag | Macro               | Marked through                                    |
|-----|---------------------|---------------------------------------------------|
| 0   | `SP_GC_ROOT`        | `sp_gc_mark` -- a direct GC pointer                 |
| 1   | `SP_GC_ROOT_RBVAL`  | `sp_mark_rbval` -- the pointer lives in a union at a nonzero offset, and only for the STR/OBJ/BIGINT tags |
| 2   | `SP_GC_ROOT_STR`    | `sp_mark_string` follows a recognized byte view to its owner |

`SP_GC_SAVE()` snapshots the root depth for a whole function; `SP_GC_RESTORE()`
returns to it.

Tag 3 -- the one value tags 1 and 2 never combine to -- is a **root frame**
(`SP_GC_ROOT_FRAME`): one entry standing for all of a generated function's
roots. It points at a stack struct headed by `sp_gc_frame_hdr`, followed by
`nv` `sp_RbVal` slots that are the homes of the function's boxed temporaries
and `np` entries in the encoding above for the locals rooted at function scope.
The emitters still write one `SP_GC_ROOT*` per local; a pass over the finished
function (`gc_frame_build` in `src/codegen.c`) moves what it can prove
function-scoped into the frame, because each per-root form cost the C compiler
an address-taken slot, a bounds check and a cleanup pop duplicated on every
scope exit -- a third of clang's unoptimised IR and most of its peak memory on
a large generated TU. The frame is zeroed once at entry, popped by
`SP_GC_SAVE`'s cleanup, and a slot keeps its last value until the function
returns: an over-approximation of liveness, never an under-approximation.
Roots the pass leaves in place (a root nested inside a loop body, a temporary
whose address is taken, the runtime's own C) keep the per-root macro; the two
forms coexist. `--no-root-frame` keeps every root in the per-root form, for
bisecting.

Comparing Struct or Data members can allocate boxes for by-value fields
(such as Complex). The comparison retains each new box while boxing the
other member and until the comparison returns. Rooting the containing
Struct or Data alone does not retain those separate boxes.

A frozen-object error keeps both the receiver and its message prefix
rooted while inspecting the receiver. The prefix can be allocated when
the object's class is determined at runtime.

Beyond the root array the mark walk consults three hook groups, in this order:

1. **fibers** -- `sp_gc_mark_suspended_fibers_hook`: the saved root stacks of
   the fibers whose C stacks are live, the running fiber's resumers (each
   suspended inside `#resume`, waiting) and the worker's root fiber. Every
   other suspended fiber is an object like any other: it lives while
   something refers to it, and its saved roots are marked when it is scanned
   (`sp_Fiber_scan`), so an object that owns a suspended fiber whose block
   captures it is collectable (#4525). A fiber that has just published its
   roots is remembered (`sp_gc_wb`), since the snapshot may name young
   objects that only its scan reaches now;
2. **globals** -- `sp_gc_mark_globals_hook`, installed by the *generated*
   translation unit, which owns state the collector cannot see: the regexp
   match registers, `ARGV`, `$0`, the in-flight exception stack, reassigned
   standard streams, and the top-level `main` object. Its Object scan marks
   the instance variable table and the values it holds;
3. **the mark stack drain**, below.

The root array holds `SP_GC_STACK_MAX` (65536) entries -- a 512 KB static
buffer, and the dominant static allocation in a minimal binary. Embedded
targets can shrink it with `-DSP_GC_STACK_MAX=<n>`, passing the same value when
building `lib/sp_gc.c` and the generated TU. **Overflow is silent and drops the
root**, which is a use-after-free; the bound is a real constraint, not a
formality.

## Marker bytes

`sp_gc_mark` reads `((unsigned char *)obj)[-1]` and dispatches:

| Byte | Meaning | Action |
|---|---|---|
| `0xfe`, `0xfa` | bytes allocated with the handle, mutable or frozen | follow the owner link |
| `0xfd` | the handle's current payload | mark the owner |
| `0xf9` | a retired payload still visible through a view | stamp the view and mark the owner |
| `0xff`, `0xfb` | static bytes | skip |
| `0xf1` | frozen bytes | skip on the object path. A typed String root can follow an adjacent owner |
| *anything else* | a GC object | read its object header and mark it |


The last row is unconditional: any pointer that reaches `sp_gc_mark` and does
not carry a known marker is **treated as a GC object**, its header read out of
whatever happens to precede it, and its `scan` function pointer called.

That is why raw and aliased pointers reaching a scanned slot are the
collector's recurring failure mode. A static singleton, a borrowed buffer, an
interior pointer into a string -- each fabricates a header and jumps through
garbage. Two properties make it hard to find:

- **Whether it faults is luck.** The byte before the object is whatever the
  linker put there. On Linux the fabricated `scan` is frequently NULL and the
  walk simply returns; the same binary shape crashes on macOS.
- **The corruption is silent when it does not fault.** A mark word written into
  rodata, or into a neighbouring allocation, shows up much later.

`SPINEL_GC_VERIFY=1` is therefore the reproducer for anything on the mark path,
not the segfault. It checks every marked object against the live registry and
reports the root group (`phase=root|fibers|globals|scan`) and the object before
invoking anything. New tests for this family assert under the flag; without it
they pass against the broken build.

**One kind of GC object is static.** A frozen String literal is one
`sp_StringLiteral` (`lib/sp_types.h`): a GC header and the handle, defined
statically per literal the unit names (`fzl_emit_defs`), and by the runtime
for `nil.to_s`, `true.to_s` and `false.to_s`. It reaches `sp_gc_mark` through
the last row of the table, as any GC object does, and its header is a real one,
written by `SP_STRING_LITERAL_INIT`. The collector never allocated it, so no
list holds it and no sweep, finalizer or recycle reaches it. It holds nothing
collectable either, since its bytes carry `0xf1`. Its header says the rest:

- `next` points at the header itself, which no listed header does. That is how
  the `SPINEL_GC_VERIFY` registry check (`sp_gc_obj_registered`) recognises it.
- `old`: a minor mark returns at it, and a major mark stamps it and stops, since
  it has no `scan`. This saves the minor mark.
- `pinned`: it takes no entry in the sticky remembered set.
- `frozen` is the String's frozen bit. Its size is 0, so it counts nothing to
  the live bytes.

A store of it takes no write barrier (`wb_value_never_young`, `src/codegen.c`).
The barrier exists so that a minor mark finds a young object an old holder now
points at, and an object that is never freed and holds nothing needs no mark to
survive. The tests pin the self-link: without it, the verifier aborts at the
first literal it marks. They do not pin `old` or `pinned`, which only save work.

## Marking

A minor mark traces young objects through roots and the old holders
recorded by the write barrier. A full mark traces the whole reachable
heap. `SPINEL_GC_MINOR=0` makes every mark full.

Marks use a 27-bit generation stamp (`sp_gc_mark_gen`). On wrap,
the collector clears the heap's stamps before it reuses them.

Tracing uses an explicit mark stack of 65536 entries. When it is full,
`sp_gc_mark` calls `scan` directly instead -- correct, but recursive, so a very
deep object graph falls back to the C stack.

## Sweeping

Each worker owns a young list. Every collection sweeps young:
dead objects are freed (through `recycle` if the type supplies one, else
`finalize` + `free`), survivors are promoted onto the single shared old list.
The old list is walked only on a **full** cycle, every 8th
(`SP_GC_FULL_INTERVAL`). Between fulls an old object that dies is reclaimed
late -- delayed reclamation, not a leak.

## Thresholds

The collection floor defaults to 256 KB and scales with the worker pool.
`SPINEL_GC_THRESHOLD_KB` sets the base and `SPINEL_GC_THRESHOLD_OBJ_KB`
overrides it. Stress level 1 starts with a 2048 B floor. Starting a worker pool
scales the floor by the pool size and rereads any explicit threshold override.
Level 2 sets the floor to zero for every allocation point that can collect.

After a collection that frees less than a quarter of the previous bytes,
the collector doubles the previous live count for the next threshold.
Otherwise it takes twice the surviving bytes, with the initial floor.
Object handles and their String payloads contribute to the same count.

## Threads

Under `SP_THREADS` collection is **stop-the-world**. `sp_stw_collect`
(`lib/sp_sched.c`) raises the safepoint flag, wakes idle workers so they park
and publish their roots rather than sit through the collection, waits for every
other worker, collects, and releases. A re-entrancy guard covers a finalizer
that allocates past the threshold during the sweep: the collector already holds
exclusive access, so that allocation proceeds rather than parking the collector
on itself.

Two things about the allocation path are there for measured reasons, not
tidiness:

- **Young lists are per worker and cache-line padded.** Removing the CAS on a
  shared head was not enough on its own -- adjacent workers' 8-byte slots shared
  a line, and the false sharing kept object-heavy parallel allocation from
  scaling. One padded line per worker isolates them.
- **The live-byte counter is batched.** It stays a single shared relaxed atomic
  (the collector's recompute and `GC.stat` both need one authoritative total),
  but each worker accumulates privately and flushes every 16 KB. A shared
  read-modify-write per allocation measured about 13x on the counter alone at
  four workers. The trigger then lags the true total by at most
  quantum x workers, which is bounded overshoot for a heuristic.

## What a program can see

```ruby
GC.start     # collect now
GC.stat      # a Hash of counters
GC.compact   # accepted, but see below
```

`GC.stat` reports `bytes`, `old_bytes`, `threshold`, `cycle`, `full_runs`,
`remembered`, `remembered_peak` and `heap_live_slots`. String handles and
their collected payloads contribute to the object heap's counters.

`GC.compact` is accepted and collects. **Nothing moves** -- the collector is
non-moving, and the method exists so that code written for CRuby does not have
to be edited.

Environment variables:

| Variable              | Effect                                                                 |
|-----------------------|------------------------------------------------------------------------|
| `SPINEL_GC_STRESS`    | drops the thresholds to 2048 B, so nearly every allocation collects     |
| `SPINEL_GC_STRESS=2`  | collects at every allocation, poisons what dies and keeps it out of reuse; see "A lost object, made visible" |
| `SPINEL_GC_VERIFY`    | registry check on every mark, plus a SIGSEGV/SIGBUS reporter naming the phase and object |
| `SPINEL_GC_PHASES`    | adds two `[gcph]` lines: collector time split into mark / old sweep / slot sweep / remembered clear / trim, and the mark split again into roots / fibers / globals / scan. Named as the collector's own comments and `sp_gc_dbg_phase` name them; arms the reporter on its own |
| `SPINEL_GC_MINOR`     | generational MARK: a non-full cycle walks the young objects and the remembered set instead of the whole live graph. Opt-in; see below |
| `SPINEL_MAX_HEAP_MB`  | RSS ceiling, checked at GC trigger points against `/proc/self/statm`; Linux only, off by default |

## A lost object, made visible

An object the roots lost -- held in a C temporary across an allocation, or by
an old object the write barrier did not record -- is freed by the first
collection that runs while it is lost. The program usually answers right
anyway, for three reasons, and `SPINEL_GC_STRESS=1` leaves all three standing:

- **Nothing collected in the window.** Level 1 collects at every object
  allocation only once 2 KB of objects are live, and once per 2 KB of young
  strings. A short test never gets there.
- **A freed slot keeps its bytes.** The sweep clears bitmap bits and writes
  nothing, so a read through the lost reference returns what was there.
- **A freed slot is reused.** The next allocation of its size class takes it,
  the lost reference names a live object again, and `SPINEL_GC_VERIFY` has
  nothing to say: the registry check passes, and the program reads or writes a
  stranger's fields.

`SPINEL_GC_STRESS=2` takes the three away:

- the threshold is zero, so every allocation point that can collect does;
- what a sweep or an explicit free (`sp_slab_free`) releases is filled with
  `0xdb` and quarantined: its pin bit keeps the allocator off the slot, and a
  bitmap of its own makes `sp_slab_is_live` answer no;
- the verifier's registry check runs on every mark. Its walks of the whole
  heap do not, since a collection here is per allocation; `SPINEL_GC_VERIFY=1`
  beside it adds them, and the slot's history to the report.

A lost object then shows in one of three ways, each of them a failure rather
than a chance:

- the mark reaches it, through the root registered too late or the holder the
  barrier missed, and the collection stops with the phase and the holder:

  ```
  *** SPINEL_GC_STRESS: the mark reached a freed slot ***
    obj = 0x7fe913020070   phase = root   ctx = 0x7ffce5ad8880
    slab: chunk wid=0 cls=2 (64 B) in_use=1 slot=1 epoch=3 ... pin=1
    freed: scan=(nil) size=56
  ```

  A swept object keeps its `sp_gc_hdr` so that this can name its scan hook
  and size; a block freed explicitly is poison throughout;
- the program reads a pointer out of it and faults on an address of `0xdb`
  bytes (the reporter says `phase = ?`: the fault is the program's, not the
  mark's), or copies that pointer into a live object, where the next mark
  faults on it with `phase = scan`;
- the program reads a number or a string out of it and prints `0xdb` bytes
  where the answer was, which a test's expected output catches.

A block the allocator hands out unzeroed (a raw payload) holds
`0xdb` too where it used to hold a dead block's bytes, so a read of memory
nobody wrote shows the same way.

Any test runs under it as it is, and so does the corpus:

```
SPINEL_GC_STRESS=2 ./a.out
SPINEL_GC_STRESS=2 make test-corpus GATE_CACHE=0
```

What it costs is a collection per allocation: a program that allocates N times
collects N times, and a threaded one stops the world each time. It is for test
programs, and one with a long allocating loop needs more than the harness's
ten seconds. The quarantine holds 1 MB (`SP_SLAB_QUAR_MAX`) and is let go
whole at the next collection once it is over, so a slot is reused only after
its whole neighbourhood has been swept again. A run without the variable pays
one not-taken branch per bitmap word the sweep frees from and one per explicit
free; no inline allocation path changed.

What it does not see: a block larger than the largest size class (2 KB) is
malloc's, and neither poisoned nor kept; so is every block under
`SPINEL_GC_SLAB=0`. A reference that outlives the quarantine is back to luck.
`gc-stress-test` holds the level to both halves of this: a host that loses an
object and a string on purpose must print the poison and stop in the root
phase, and programs that root what they use must answer the same under it.

## The generational mark, and why it is opt-in

There **is** a write barrier now, and `SPINEL_GC_MINOR=1` turns on the mark
that uses it: `sp_gc_wb` records an old object that takes a young reference,
and a non-full cycle marks from the roots plus that remembered set rather than
tracing the whole live graph. It is off by default, and the reason is a
measured trade rather than doubt about the code:

- it wins where the retained heap is **large and stable** -- a 440 MB live
  object set here is ~5% faster with 4% less RSS, and the Campfire port at
  #4386 measured +4.0% on its room page;
- it loses where the shape is **churn with a small live set** -- optcarrot is
  ~2.7% slower, and a 50-program LangArena run in August was 13% slower overall
  (BWTEncode 4.3x, AStar 2.3x) against 27% less peak RSS.

Default-on was tried once and reverted (01bc08c8). The correctness side has
moved since: `gc-minor-test` holds one leg per barrier gap that shipped, and
`SPINEL_GC_VERIFY_GEN=1` names the holder of anything a barrier missed. The
performance side has not moved, which is why the flag is still a choice an
application makes rather than a default: an application that retains a lot can
measure the win and take it.

## Limits, and where the next work is

**The mark does not parallelize.** The young sweep is handed to the parked
workers, one slot each; the mark runs on the collector thread alone. Its cost
tracks the number of IN-FLIGHT fibers, not the worker count -- every live
fiber's saved roots are walked serially by `sp_mark_suspended_fibers` -- so on
a server it grows with concurrency: mark went 0.317 -> 0.812 ms/request from
4 to 64 connections with the worker count held fixed, while slot sweep stayed
flat (#4384). A terminated fiber already costs nothing there (its snapshot is
dropped at termination, since it points into unwound frames).

Before slicing the fiber list to the parked workers, read the mark split that
`SPINEL_GC_PHASES` prints, because "mark grew" has two causes and they want
different answers. `fibers` is the serial walk of every live fiber's saved
roots -- that is what slicing would parallelize. `scan` is the trace that
drains what the roots found, and it grows for a different reason: those fibers
hold live objects, so there is more graph. On a synthetic with the worker count
held fixed and only the number of parked deep fibers varied, `fibers` went
0.000s -> 0.006s from 1 to 128 while `scan` went 0.006s -> 0.006s at 32 and
only then began to move -- so at low fiber counts the split is almost all
trace, and slicing would buy nothing.

What the slicing itself costs is smaller than it first looks. `h->marked` is
written with the same value by any worker, and a racing check-then-set costs at
most scanning one object twice, which is idempotent -- it is not a correctness
barrier. The shared mutable state is the mark stack alone (`sp_gc_mark_stack`,
`sp_gc_mark_top`, and its realloc growth), so per-worker stacks or an atomic
top without realloc is the shape. Parallelizing the `scan` phase as well needs
work stealing, which is a different size of change.

Also absent:

- **compaction** -- nothing moves, so fragmentation is the allocator's problem;
- **incremental or concurrent marking** -- the pause is the whole mark;
- **`ObjectSpace.define_finalizer`** -- `finalize` and `recycle` are internal
  hooks on `sp_gc_hdr`, with no Ruby-level surface.

The write barrier that section used to call the next step now exists (see
above). The warning it carried is worth keeping, because it turned out to be
right: the barrier sits on every ivar and container store, which is the hottest
code the compiler emits, and the frame-rate figure this project gates on is
sensitive enough to code layout that a real cost and a layout accident are easy
to mistake for each other. Settle how a change will be measured before writing
it -- interleave the arms, and prefer flipping an environment variable on ONE
binary over comparing two builds.
