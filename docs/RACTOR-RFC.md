# RFC: A Minimal Ractor for Spinel

Status: **experimental / Milestone 6** (working; ported to the C compiler in
`src/`). Deep-copy message codec spans immediates / Symbol / String / Array /
Hash / user-objects; `Ractor.new` spawn arguments; compile-time `@@cvar`/`$gvar`
isolation; and a shared `Ractor::Port` channel. This document is both the design
rationale and the record of what currently ships. It proposes whether to
graduate to a full implementation (see *Path forward*).

## Why this is cheap here

Spinel's runtime (`lib/sp_runtime.h`) keeps essentially all mutable state in
file-scope `static` globals: the GC heap, the string heap, the dynamic symbol
table, the exception/`throw` `longjmp` stacks, the regex captures, the fiber
pointer, the lambda arena, the at-exit hooks, and the per-class object pools.
Generated code emits `$globals` and `@@cvars` as statics too. That is exactly
why Spinel is single-threaded.

Ractor's whole premise is **isolation + message passing**. Spinel's
"all state is a static global" shape maps onto that almost for free:

> Promote the per-execution-unit mutable globals to thread-local (`__thread`),
> run each Ractor as a pthread, and each Ractor gets its own private GC heap /
> string heap / symbol table / exception stack. Because each heap is private,
> **no global GC lock is needed** — Ractors collect independently. The
> compile-time-`const` class/dispatch tables stay shared and read-only.

A Ractor is, structurally, "a Fiber body run on a pthread, with mailbox/yield
queues instead of resume/yield slots, deep-copied at the boundary."

## The state partition

```
lib/sp_runtime.h globals, reclassified:

(a) __thread  — per-Ractor mutable, NO lock
    GC core (heap/bytes/threshold/old_heap/cycle/buckets), the GC root window
    (a lazily-malloc'd __thread pointer so it doesn't commit 512 KiB of TLS),
    the mark stack, the string heap + lcache, the exception/catch longjmp
    stacks, the regex captures ($~), the fiber root/current/list (both #ifdef
    branches), the lambda arena, the at_exit hooks, and the per-class pools.

(b) shared const, read-only — NO lock
    sp_class_names[] and all dispatch/vtable tables; the static portion of
    sp_sym_names[]; rodata string literals; install-once function-pointer
    hooks (sp_obj_hash_hook); sp_gc_verify.

(c) process-global WITH a lock — genuinely shared (all NEW infra)
    each Ractor's mailbox + outgoing queue (a mutex + condvar each) and the
    reference-counted control block. NOT the GC (private heaps), NOT malloc
    (glibc is already thread-safe).
```

`sp_thread_init()` wires each thread's `sp_fiber_current` to its own synthetic
root fiber (the address of a `__thread` var is not a constant expression, so it
can't be a static initializer). It runs on the main thread via an ELF
constructor and on each Ractor thread from its trampoline. The root array and
mark stack self-allocate lazily on first use.

## Architecture & data flow

```
Ractor.new { ... }  ──pthread_create──►  trampoline:
                                           sp_thread_init()  (fresh TLS heap + fiber root)
                                           set top-level setjmp landing pad
                                           run the block body (_ractor_body_N)
                                           close outbox; release ctrl refcount

parent: r.send(v) / r << v  ── serialize ─►  inbox  (mutex+condvar) ─► Ractor.receive
child:  Ractor.yield(v)     ── serialize ─►  outbox (mutex+condvar) ─► r.take : parent
```

**Boundary = serialize to a heap-neutral buffer.** A sent value is a pointer
into the sender's private heap; the sender's next GC would free it. Copying
directly into the receiver's heap is unavailable, because `sp_gc_alloc` writes
the *running* thread's thread-local GC slots — allocating into another heap
would need a per-heap lock on every send, reintroducing the global GC lock this
design eliminates. So the sender serializes into a malloc'd, pointer-free
buffer; the receiver rebuilds via ordinary `sp_gc_alloc` into its own heap; the
buffer is freed after. This matches CRuby's default deep-copy `send`.

## Public API (conforms to current CRuby Ractor)

```ruby
r = Ractor.new do
  x = Ractor.receive      # pop from this Ractor's inbox (also: Ractor.recv)
  Ractor.yield(x * 2)     # push to this Ractor's outbox
end
r.send(21)                # push to r's inbox (also: r << 21)
puts r.take               # => 42  (pop from r's outbox)
```

Default deep-copy semantics. A Ractor block must be self-contained: accessing
any outer local variable is a compile-time `Ractor::IsolationError` (CRuby
raises `ArgumentError` at runtime; the diagnostic names every offending local,
e.g. `block accesses outer variables (a, b)`). Shareable **constants** remain
accessible, like CRuby. The block's implicit return value is delivered to the
first `take`, as a final implicit yield after any explicit `Ractor.yield`.

`Ractor.make_shareable(obj)` deep-freezes a value and returns it;
`Ractor.shareable?(obj)` reports whether a value is deeply immutable
(immediates, `Class`, `Ractor::Port`, a frozen `String`, or a frozen
Array/Hash/object all of whose reachable values are shareable). A sent
shareable value is still deep-copied at the boundary -- genuine by-reference
sharing across the private per-Ractor heaps is separate future work; for
immutable data a copy is observationally equivalent.

## Implementation map

(The active compiler is the C reimplementation under `src/`; the runtime is
split across `lib/sp_gc.c`, `lib/sp_fiber.c`, and the header `lib/sp_runtime.h`.)

- **`lib/sp_gc.{c,h}`, `lib/sp_fiber.{c,h}`, `lib/sp_runtime.h`** — `__thread` on
  every per-execution-unit mutable global (collector heap/roots/mark-stack/
  byte counters; fiber root/current/list; string heap + literal cache;
  `SP_POOL_DEFINE` pools; exception + catch stacks; regex match state; at_exit;
  lambda arena), plus `sp_fiber_thread_init()` (a main-thread constructor wires
  the main thread), `sp_gc_thread_teardown()` (frees a Ractor's heaps on exit),
  and the Milestone-2 message codec `sp_ractor_serialize`/`_deserialize`
  (installed into the runtime hooks by `sp_re_init`).
- **`lib/sp_ractor.{c,h}`** — `sp_Ractor` + mutex/condvar FIFO mailbox/outgoing
  queues, the pthread entry trampoline, and the serialize/deserialize hook
  plumbing. The generated body installs a top-level `setjmp` rescue pad so an
  uncaught Ractor exception terminates one Ractor rather than the process.
- **`src/analyze*.c`** — `TY_RACTOR`; `"Ractor"` as a builtin constant; inference
  arms (`Ractor.new → ractor`, `receive`/`take → poly`, `yield → nil`,
  `send`/`<< → ractor`).
- **`src/codegen*.c`** — `c_type TY_RACTOR → "sp_Ractor *"` and the scalar-return
  list; `emit_ractor_new` (Fiber-body-shaped, rejects captures/self with a
  compile-time `Ractor::IsolationError`); the class/instance dispatch arms.
- **`common.mk` / `src/main.c` / `Makefile`** — `-pthread` in `LDFLAGS`, in the
  in-process `cc` link driver, and on the new `sp_ractor.o` build rule.

## Deliberate divergences (where "minimal" buys simplicity)

1. **Value codec covers immediates, Symbol, String, Arrays, and Hashes** (poly
   + typed variants), deep-copied recursively through a heap-neutral malloc'd
   blob (`sp_ractor_serialize`/`_deserialize`, installed via a hook because the
   rebuild allocators are static to the generated TU). Plain objects, Procs,
   Fibers, and IO raise `Ractor::Error` at the boundary — extending the codec to
   user objects is the main follow-up. Symbols travel by name and are
   re-interned in the receiver's per-Ractor table; hashes are normalised to a
   poly hash on rebuild. The same codec seeds `Ractor.new(args) { |params| }`
   spawn arguments.
2. **FIFO mailbox / outgoing queue** (mutex + condvar, grows on demand), so a
   Ractor may `receive` several messages and a sender need not block per slot.
3. **`@@cvars` / `$globals` accessed inside a Ractor are rejected at compile
   time** with `Ractor::IsolationError`, mirroring CRuby (which raises on
   cross-Ractor class-variable / global access). Special, thread-local or
   read-only globals (`$~`, `$stdout`, regex/status specials — not user-declared)
   are allowed. The globals themselves remain `__thread` in the runtime; the
   compile-time check just prevents the silent-empty-value footgun.
4. **`Ractor#send` with a bare String/Symbol *literal*** is parsed as a
   reflective send (`recv.send("m")` → `recv.m`, a whole-program parser
   behaviour), so send messages via `r << v` or a local (`r.send(v)`); literals
   like `r.send(42)` / `r.send([..])` are unaffected.
5. **Outer-local captures are rejected, matching CRuby.** A Ractor block may
   not access any outer local variable (`self` included) -- a compile-time
   `Ractor::IsolationError` naming every offender. CRuby 3.4 does the same
   (an `ArgumentError`), even for an `Integer`: shareable *locals* are not
   auto-captured, only shareable *constants* are accessible. (An earlier RFC
   draft proposed copying shareable locals by value; that would make Spinel a
   permissive superset of CRuby, so it was dropped in favour of the faithful
   rejection.) Spawn args + block params (M3) and the block's return value
   delivered to `take` (#1453) are the supported ways to get data in and out.

## Verification

- The `__thread` storage-class conversion is behavior-neutral: the full
  existing test suite passes unchanged, single-threaded (896 pass / 0 fail /
  0 error with the two Ractor tests; CI green on ubuntu-gcc/clang + macOS-clang).
- `test/ractor_basic.rb` exercises `Ractor.new` / `receive` / `yield` /
  `send` / `<<` / `take` end-to-end and prints `42` twice; `test/ractor_messages.rb`
  round-trips String / Symbol / poly+typed Array messages.
- Clean under `SPINEL_GC_VERIFY=1`, **ThreadSanitizer** and **AddressSanitizer**
  (run binaries under `setarch -R` for TSan on this aarch64 box), and **valgrind
  memcheck + helgrind** report 0 leaks / 0 data races. Each Ractor's private GC
  and string heaps are freed on thread exit (`sp_gc_thread_teardown` +
  `sp_gc_str_teardown_hook`); the `sp_Ractor` control structs are retained in a
  process-lifetime registry (a detached worker plus a GC-untracked handle have
  no safe mid-run free point) so they stay reachable rather than leaking.
- The isolation rule rejects a block that captures a mutable outer local with
  `Ractor::IsolationError`.

## Path forward (if this graduates)

Done since the original RFC: the deep-copy codec now spans strings → symbols →
arrays → hashes → **user objects/structs** (M2/M3b/M4, recursively); the mailbox
is an unbounded FIFO; `Ractor.new(args){|p|}` spawn arguments are deep-copied in
(M3a); `@@cvar`/`$gvar` access inside a Ractor is rejected at compile time (M5);
and **`Ractor::Port`** provides a process-shared channel (`new`/`send`/`receive`/
`close`), passed across the boundary *by reference* so producers and consumers
on different Ractors rendezvous through it (M6). The block's return value is
delivered to the first `take` (#1453), and `Ractor.make_shareable` /
`Ractor.shareable?` provide the deep-freeze + shareability predicate (#1455).
Remaining: by-reference sharing of shareable values across the private heaps
(today even a frozen value is deep-copied on `send`), object-graph cycles in the
codec (acyclic assumed today), and the Ruby-3.5 Ractor-as-Port API reshaping.
None change the core partition.

### API sharp edges (operator/method overload collisions)

- **`Ractor#send` / `Ractor::Port#send` with a bare String/Symbol *literal*** is
  parsed as a reflective send (`x.send("m")` → `x.m`, a whole-program parser
  behaviour). Send via `x.send(var)` or an Integer literal; `x.send("lit")` is
  reflective.
- **`port << v`** can be mis-inferred as an Array push (the `<<` push-promotion
  heuristic types the receiver as an array). Use `port.send(v)` for ports.
