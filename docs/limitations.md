# Spinel limitations: what an AOT compiler can and cannot do

Spinel is a whole-program **ahead-of-time** compiler: it reads the entire
program, infers a static type for every value, emits C, compiles it, and runs
the binary. There is no Ruby interpreter, parser, or type-inference engine in
the running program -- it is just C. That model is what buys the speedup, and it
is also the source of every limitation below.

This document is the honest catalogue. It is organized by *kind* of limit:

- **Fundamental** -- incompatible with whole-program AOT; will not change without
  abandoning the model (e.g. bundling an interpreter).
- **Partial / relaxable** -- genuinely limited today, but additively fixable.
- **By design** -- a deliberate, documented choice; the intentional CRuby
  deviations are catalogued under [By design](#by-design-deliberate-choices)
  below.
- **Now supported** -- things that are *not* limits (corrects older write-ups
  that described an earlier version of the compiler).

A limit the compiler meets is a *refusal*: a `spinel: FILE:LINE: ...` line on
stderr naming the construct. One run reports every refusal in the program
(each abandons the method it is in and the compile goes on to the next), then
fails once with the count and writes nothing, so a program brought over from
CRuby learns its whole list in one compile. `--emit-types` carries the same
refusals in its `diagnostics` array with `"severity":"error"`.

---

## Fundamental limits (inherent to AOT)

These need a runtime parser, a runtime metaobject protocol, an allocation
registry, or stack reification -- none of which exist in a flat compiled binary.

| Feature | Behaviour | Why it's fundamental |
|---|---|---|
| `eval` / `instance_eval("str")` / `class_eval("str")` | unsupported | needs a runtime parser + type system. (Block forms -- `instance_eval { }` -- DO work; the block is compiled.) Code in a branch a `RUBY_ENGINE == "..."` check rules out (`if RUBY_ENGINE == "spinel" ... else eval(...) end`) is dropped before analysis, so a library can keep an eval backend for other engines. |
| `method_missing` | not dispatched (defining it warns at compile time) | every call site is a direct C call; an undefined-method call can't fall back to a per-receiver hook. The method is still callable explicitly. |
| `define_method` with a runtime-computed name/body | only literal names work | a runtime-built method has no compiled body |
| `ObjectSpace` (`each_object`, `count_objects`) | unsupported | no class-keyed allocation registry; the GC tracks bytes, not a live-object index. `define_finalizer` / `undefine_finalizer` *are* supported (the collector watches the object; the callable runs at the first safe point after the collection that frees it -- a method entry, a loop back-edge, `GC.start` -- or at exit) |
| `TracePoint` / `set_trace_func` | unsupported | require an interpreter loop to hook |
| `binding` as an object | unsupported | reifying the local scope needs a runtime name->slot table; locals are C stack slots. `binding.local_variable_get(:x)` with a **literal** name *is* supported -- it resolves to the known slot at compile time |
| Refinements (`refine` / `using`) | no-op / unresolved | scope-keyed dispatch is incompatible with direct C calls |
| `callcc` / `Continuation` | unsupported | multi-shot full-stack capture has no flat-C analogue |
| `Class.new(parent) { ... }` (runtime class) | unsupported | the class graph is baked at compile time |
| Singleton methods (`def obj.m`, `class << obj; def m; end; end`, `obj.define_singleton_method(:m) { }`, `obj.extend(Mod)`) on a receiver whose creation site is **not** visible | unsupported | these DO work when the receiver is a constant or a local whose only write is `<UserClass>.new(...)`: the object gets a synthesized anonymous subclass carrying the methods, which is the AOT form of CRuby's hidden singleton class. What is left out is a receiver spinel cannot trace to one `.new` (a factory return, a loop, a conditional), and one whose class has no subclassable layout: `Object.new` / `BasicObject`, a builtin (String, Array), a Struct or Data, an exception. Those are refused at compile time, naming the Ruby line, when the body needs a `self` (its own `@ivar`, or `self`); a body that needs neither compiles as an ordinary function and is simply never reached as a method |
| `Object#singleton_class` as an OBJECT (and `Class#attached_object`) | unsupported | the singleton class above is synthesized, not reified: there is no runtime class object to hand back. `class << obj` as a *definition* form works -- see the row above. `singleton_class.prepend(Mod)` / `singleton_class.include(Mod)` as a statement of a class or module body (activesupport's const_missing hook on Enumerable) is read as `extend Mod`, the precedence between Mod and the class's own singleton methods aside |
| Runtime structural mutation of a class through an explicit receiver (`Klass.include(M)`, `Klass.attr_accessor(...)`, `Klass.define_method(...)` outside the class body) | unsupported | the class graph, ancestor chain, and method/ivar layout are baked at compile time; the same declarations *inside* a `class` body work |
| A call through an `@ivar` before the method that assigns it has run (`@store[k] = v` ahead of a `reset` that sets `@store = {}`) | a release build dereferences the unset slot and crashes (SIGSEGV); a `-g` / `--debug` build raises CRuby's `NoMethodError` (`undefined method '[]=' for nil`) | the test would stand in front of every call through such an ivar, and state a `reset` or `setup` method assigns is often the hottest there is (optcarrot lost 10-20% to it), so only the debug build carries it. An ivar the program fills only by memoization (`@c ||= {}`) is guarded in both builds |
| General reflection (`methods`, `instance_variables`) and `instance_variable_get`/`set` with a **non-literal** name | unsupported | ivars are C struct offsets with no name→offset table; DCE strips method names. A **literal** `instance_variable_get(:@x)` / `instance_variable_set(:@x, v)` *is* supported -- it resolves to the known struct offset, like `send(:literal)` below. |
| User-defined `#hash` / `#eql?` for hash *keys* | not dispatched (identity probe) | the hash machinery can't call back into a user method per key |
| A method that **uses its block** (`yield` or `block.call`) **and recurses into itself** (`def rec(n, &b); ...; rec(n-1, &b); yield n; end`) | compile error (loud, was a hang / undefined-symbol) | a block-using method is inlined at each call site (there is no standalone function that takes the block), so a self-call inlines its own body unboundedly -- the runtime base case is invisible at compile time. Recursion *through a yielded block* (`with_state { with_state { } }`, finite source nesting) does work |
| `Monitor#class` | reports `Thread::Mutex` | a Monitor IS a mutex here, with reentrancy switched on per object, and the class name for a `TY_MUTEX` value is decided at compile time from the type rather than read off the object. `#synchronize` (including reentrant use), `#try_enter` and mutual exclusion across threads all behave as CRuby's do; only the name differs. `Monitor#new_cond` / the `MonitorMixin` module are not modelled. |
| `require` of stdlib `.rb` that leans on metaprogramming / C extensions (e.g. `json/pure`, the `require "time"` parsing extensions like `Time.parse` / `Time.strptime`) | unsupported | such stdlib code runs off the AOT path. A `require` is resolved at parse time by splicing a bundled `lib/X.rb`; the libraries that ship this way -- `set`, `forwardable`, `optparse`, `erb`, `csv`, `pathname`, `stringio`, `strscan` -- do work. The built-in `Time` class (`Time.now` / `at` / `local` / `utc`, plus `strftime` / `zone`) works *without* any `require`; only the `require "time"` string-parsing additions are missing. |

**`net/http` / `uri`.** An HTTP/1.1 client with `Connection: close`, one
request per connection -- a second request inside one `Net::HTTP.start` block
reconnects transparently, as CRuby does, rather than failing, but the
connection is not reused: no keep-alive, no pipelining, no HTTP/2, no proxy,
no cookie jar and no automatic redirect following (a 3xx comes back as the
response it is, with its Location). Chunked transfer decoding is there;
content-encoding is not. `URI` parses http, https and a bare form; there is no
URI::FTP or the scheme registry behind it. An https request needs the
`openssl` package below.

**TLS / `openssl`.** The `openssl` package binds the system libssl and
provides `OpenSSL::SSL` only: `SSLContext`, `SSLSocket`, `SSLError` and the
`VERIFY_*` constants, which is what an outbound HTTPS client reaches.
`OpenSSL::Digest::SHA256` / `SHA1` / `MD5` and `OpenSSL::HMAC.hexdigest` are
there, over the runtime's own crypto rather than libssl -- class-method forms
only, and no HMAC-MD5. `Cipher`, `PKey`,
most of `X509`, and the incremental digest object API are not there, and a
program that names them fails to compile rather than at run time. Spinel
implements no TLS and bundles no trust anchors: the chain is validated against
the operating system's store, so a CA it stops trusting stops being trusted
here on an OS update. The package exists only where libssl's headers were
present at build time.

The require-gated stdlib Spinel *does* provide (`StringIO`, `IO#winsize`,
`Time#iso8601`, ...) requires its `require`, matching CRuby; an unsatisfiable
`require` is a compile error. This is opt-in today via `--require-gate` (or
`SPINEL_REQUIRE_GATE=1`); `spin build` always compiles with it on.
See [require.md](require.md) for which stdlib needs which `require`.
| Mixed / non-UTF-8 encodings | UTF-8 / ASCII-8BIT only | one internal representation; transcoding tables are out of scope |
| Embedded `NUL` in general binary strings | `char *` boundary assumption | most string ops are NUL-terminated at the C boundary |

`send`/`public_send`/`__send__` with a **non-literal** name (`send(meth)`) is
partially supported: an explicit-receiver send lowers to a static dispatch over
the method names that appear as symbol/string **literals** anywhere in the
program -- `recv.send(name) → name == :a ? recv.a : name == :b ? recv.b : … :
raise NoMethodError` -- with the receiver's type and the argument count selecting
which arms resolve (the result is `poly`). A name that is not one of those
literals, or not a method on the receiver, raises `NoMethodError` at runtime. A
**computed** name (an interpolation, `to_sym`, a concatenation) sent to a user-class
receiver dispatches over everything that class answers -- its methods, readers and
writers, and the Object methods every instance has -- so the set is complete; on a
boxed receiver it covers every user class plus the program's literals. A computed
name sent to a builtin receiver (a String, an Array, ...) is still refused at compile
time. A **literal** name is fully resolved -- see below.
`public_send(*args, &blk)` with the name as the first element of a splatted
variable (activesupport's `Object#try`) dispatches too: the arms take
`*args.drop(1)`. The send's block -- written at the send or a forwarded `&blk`
-- goes to the method the name selects. A receiverless `send` / `public_send`
in a method is the `self.` form. `respond_to?(name)` with a runtime name is
answered over the same closed set (false outside it).

---

## Partial / relaxable limits

Limited today, but additively fixable; listed roughly easiest-first.

| Feature | Today | Path to relax |
|---|---|---|
| `Exception#backtrace` / `Kernel#caller` | return `[]` (class + message work) | populate frames from a compile-time call-site→source side-table (the `--line-map` map already exists) |
| `class Thread` / `class Fiber` reopenings, `Thread.attr_accessor :x` / `Fiber.attr_accessor :x` (activesupport's IsolatedExecutionState) | supported | a reopening's instance methods take the runtime handle as self, and `Thread.current` / `Fiber.current` reach them (also through a class value or a class held in a poly slot). A thread's attribute lives in its thread-local table under a private key; a fiber's in a table of the fiber's own that a new fiber does not inherit (an attribute on a fresh fiber is nil). `thread_variable_get` / `_set` / `?` share the store `Thread#[]` / `[]=` / `key?` keep: one table per thread for both, where CRuby's `[]` is fiber-local |
| `Thread` real parallelism | implemented as a true M:N runtime (no GVL): N OS workers (`min(online cores, SPINEL_WORKERS)`) run green threads in parallel over a stop-the-world GC, with real `Mutex`/`Queue`/`SizedQueue`/`ConditionVariable`. A monitor thread timeslices CPU-bound threads (~10ms quantum) so a thread looping without yielding cannot starve its siblings (it signals the worker with `SIGURG`, overridable via `SPINEL_PREEMPT_SIGNAL`). The single-threaded archive is unchanged (a non-`Thread` program is byte-identical) | the N workers run per-worker run queues with work stealing, and `Kernel#sleep` and blocking I/O are scheduler-aware (a sleeping / I/O-blocked thread frees its OS worker). preemption is taken at safepoint polls (loop back-edges), so a thread spending a long time inside a single runtime call with no poll yields only when that call returns; concurrent allocation is thread-safe (heap-lock-protected allocators, atomic heap byte counters, per-worker object pools) but every allocation still crosses one global heap lock; remaining work: fully async (signal-interrupted) preemption of such regions, and per-worker allocation buffers (TLAB) to make allocation-heavy parallel code scale. See [docs/thread.md](thread.md) |
| `Marshal` of user objects with container-typed ivars | primitives + Array + Hash + Bignum + Complex + Rational + plain user objects work, including cyclic and shared references (`Marshal.dump`/`load`, CRuby 4.8 wire format, byte-compatible for the supported subset); an object whose ivar is a *statically typed* Array/Hash (not a poly ivar) is not yet dumpable | a user object dumps/loads through a compile-time-generated per-class dispatcher. Supported ivar types: scalars (Integer/Float/String/true/false/Symbol/Bignum), `poly` (mixed) ivars, and nested user objects. A typed-container ivar would mismatch the loader's always-poly containers, so such a class raises `TypeError` on dump; value-type and Exception-subclass objects are also out of scope. Complex's components are float-only, so they round-trip as Floats |
| Mixin/inheritance lifecycle hooks (`included` / `inherited` / `extended`) | defined but not fired | emit a startup call with the literal class arg (the include/inherit graph is known at compile time) |
| Methods added to `Class` (`class Class; include M; end`, `Class.class_eval { include M }`, a `def` in either) | a class method of every class: `Klass.m`, `String.m`, and a bare `m` in any class body. Refused at compile time, naming the line: `Class.class_eval` anywhere but a top-level statement, and `class Class < ...`. On a builtin class a class-level `@ivar` of such a method is one slot shared by every builtin class | a nested or conditional addition would need the methods to exist only once it has run; a builtin class has no class-ivar storage of its own |
| External `Enumerator` -- `.each` with no block is only an Enumerator on `Array` / `Range`, not on an arbitrary user method | mostly supported | `Array#each` / `Range#each` with no block return a working external Enumerator (`#next` / `#peek` / `#rewind` / `#size`, `loop` stops on `StopIteration`). `Enumerator.new { \|y\| ... }` is a fiber-backed generator (`y << v`, `y.yield(v)`, and the bare `y.yield v` without parentheses, plus `#next` / `#peek` / `#rewind` / `#take` / `#first`, infinite generators work). `Enumerator::Lazy` over an int range (incl. endless) or int array fuses map/select/reject/filter/take_while chains terminated by `first(n)` / `to_a` / `force`. Chained block→`.to_a` forms (`each_slice(n).to_a`, `filter_map`, `map{}.to_a`) also work. |
| `Enumerable#each_entry` on a user class whose `#each` yields MULTIPLE values | yields them spread, as `#each` does, rather than packed into an array | on every builtin enumerable (Array/Hash/Range/Enumerator/Dir) `#each` yields one value per element, so `each_entry` is compiled as `each` and matches CRuby exactly. The difference only shows for a user `#each` that does `yield a, b`, where CRuby's `each_entry` hands the block `[a, b]`. Packing needs the yield arity of the user's `#each`, which is a static property of its body |
| `redo` in the block of an iterator whose emitter walks the body itself (a lazy pipeline's stages, `chunk_while`, `transform_values`, `Array.new`, ...) | refused at compile time, naming the line | `redo` re-runs the body in place, which needs a label after the block's setup. `each`, `times`, `upto`, `map`, `select`, `reject`, `find`, `flat_map`, `sum`, `inject`, `sort_by`, `uniq`, a comparator for `sort`, `min` or `max`, `bsearch`, `gsub`, `map.with_index`, the in-place filters, `map!`, `fill`, `product`, `tap`, `each_char`, `each_index`, a user `yield` and the other iterators that go through the shared body emitters place one; each remaining emitter would need the same. Compiled as a `continue`, it used to leave the block as `next` does |
| `StringIO#each_line` / `#each` / `#each_char` / `#each_byte` with NO block | `LocalJumpError`, where CRuby answers an `Enumerator` | the block forms are exact. Answering an Enumerator instead would make the method return either that or `self`, a union with no C slot. `io.readlines.each`, `io.read.each_char`, and `io.read.bytes` say the same thing and do have one |
| `StringIO#readpartial` / `#sysread` / `#read_nonblock` with a buffer argument | the data comes back as the result, but the caller's own buffer variable is not changed | the methods are plain Ruby in the stringio package, and a String parameter a package class's method changes is not yet passed by reference the way a user class's is. Through a value that may be a File or a StringIO (`def rd(io) = io.readpartial(n)` called with both) the three methods raise NoMethodError: that receiver takes the built-in IO arms, which don't see a package's plain-Ruby methods |
| `IO::Buffer` | the full in-memory API, CRuby-faithful: `new` (INTERNAL/MAPPED flags), `get_value`/`set_value`/`get_values`/`set_values` over all 18 type symbols (little/big-endian, `u64` round-trips Bignums under `--int-overflow=promote`), `get_string`/`set_string` (NUL-safe binary), `resize`/`clear`/`copy`/`size`, `slice` (live views, safe across a source `resize`), `transfer`/`free`/`dup`, `<=>`/`==`, `hexdump`/`inspect`/`to_s`, the predicates, the tiling bitwise family (`&` `\|` `^` `~` and `and!`/`or!`/`xor!`/`not!`), `locked`, `IO::Buffer.for(string)`/.string/.size_of, the CRuby exception classes (`IO::Buffer::AccessError` etc.), and the IO integration: `#read`/`#write`/`#pread`/`#pwrite` against an IO (one syscall each, answering the count, 0 at EOF or -errno; a blocking read on a socket or pipe parks the green thread, and the buffer is locked for the duration) and `IO::Buffer.map(file, size, offset, flags)` as an mmap view (READONLY / SHARED / PRIVATE; munmap'd by the finalizer; `resize` refused as for EXTERNAL). Passed to an `ffi_func` pointer argument, a buffer hands C its base address for the call ([FFI.md](FFI.md)). No `require` needed, as in CRuby. A literal type symbol compiles to a direct typed accessor (the wasm-runtime / binary-protocol hot path) | `#read` serves the bytes the IO's own stream already buffered before reading the descriptor (a `getc` followed by a `read` sees the next bytes); CRuby's reads the descriptor directly and can skip what its buffer holds. Three more deliberate divergences: `IO::Buffer.for(string)` copies (Spinel strings are immutable, so unobservable) and its write-through BLOCK form raises `NotImplementedError`; `each`/`each_byte`/`values` are block-form only (no Enumerator, as with StringIO); `get_string`'s third (encoding) argument is not accepted |
| The value of `super` in `initialize` (`c = super`, `super.frozen?`, `c = if f then super else [] end`) when the parent's `initialize` is the program's own | refused at compile time, naming the line | an `initialize` is compiled to return nothing, since `new` drops its value; one whose value a subclass's `super` asks for would return its last value instead |
| `Array#hash` (and arrays as hash keys) | unsupported | a builtin is additive, but array *keys* need the fundamental key-dispatch above |
| Sockets | TCP / UDP / UNIX-domain, as IO handles -- see below | additive: each missing class and method is its own runtime binding |
| Passing data through a named pipe (FIFO) between two threads, **on macOS** | the reader gets nothing and the program hangs; Linux answers what CRuby answers | not the open, which is what #4394 was about, and not any change since: a reader and a writer exchanging three lines through one `mkfifo` path fails on macOS against a tree with no runtime change at all (#4406), so it is the readiness path a FIFO descriptor reaches once both ends exist. A pipe (`IO.pipe`) or a UNIX-domain socket carries the same traffic and works on both. Opening a FIFO no longer stalls the other green threads on either platform |
| `class LoadError` / `class NameError` / `class Exception` … reopenings of a builtin exception class (activesupport's `core_ext/load_error.rb`, `core_ext/name_error.rb`, `Exception#as_json`) | supported | the class stays the runtime's: `raise LoadError, msg`, `LoadError.new(msg)`, `rescue LoadError => e`, `is_a?` and `e.class` behave as before the reopening (they used to build a shadowing user class, so `raise LoadError, msg` was a TypeError). The added methods take the runtime exception as self and are reached on a rescued or constructed exception and on a user subclass's instances; a bare `message` / `key` / `name` / `path` inside one is the exception's own. When several reopenings define one name (`Exception#brief` and `KeyError#brief`), a base-typed receiver is told apart by its runtime class, most-derived first in declaration order; a poly (run-time-typed) receiver does not see these methods yet |
| `p` / `#inspect` on a `Thread::SizedQueue` | prints as `#<Thread::Queue:0x...>` | A SizedQueue and a Queue share one slot type here, so the name a handle prints is the slot's rather than the object's. Mutex, Queue, ConditionVariable, Fiber and Thread print what CRuby prints for them (a Fiber or Thread with its creation site, which is `file:0` when compiled with `--no-line-map`) |
| `Fiber.new(storage: hash)` / `Fiber#storage=` with a Hash that has a default | the default is dropped: `Fiber[:missing]` reads nil | the fiber keeps the Hash's entries, not the Hash itself, so its `default` / `default_proc` don't come along; CRuby keeps the Hash |
| A fiber scheduler (`Fiber.set_scheduler`, `Fiber.schedule`, the `Fiber::Scheduler` hooks) | not supported: `Fiber.scheduler` and `Fiber.current_scheduler` are always nil | blocking IO, `sleep` and the thread primitives park the green thread on Spinel's own scheduler instead (see [thread.md](thread.md)); nothing routes them to a Ruby scheduler object yet. `Fiber#blocking?`, `Fiber.blocking?`, `Fiber.blocking { }` and `Fiber.new(blocking:)` work, and answer what CRuby answers with no scheduler set |
| `k.new(x)` where `k` is a Class VALUE and the constructor parameter is typed by its DEFAULT | `NoMethodError` where CRuby constructs | `initialize(a = 1)` types `a` Integer. A statically known `Klass.new("x")` widens that parameter, because the inference can see the call site and which class it names; a class-value call site names no class, so it seeds nothing and the parameter keeps the type its default gave it. The dispatch then has no arm for a String argument and raises. Passing an argument of the parameter's own type works, as does any constructor whose parameters are typed by their uses rather than by a default. Before this raise existed the arm was selected anyway and the argument's bits were read as the parameter's type, so the raise is the fix rather than the limitation |
| A promoted value stored into an int-typed Array (`--int-overflow=promote`) | truncated back to int64 by the store | the array's element type has to widen with the value; blanket-widening every int array costs promote mode more than it buys, so this wants a data-flow rule. Seeding the array with one value past 2^63 (or holding the state in a scalar) keeps the promotion today |

### Bound Methods read out of poly slots

A callable boxed into a poly container (`[obj.method(:m)][0].call(x)`) is
dispatched at run time, so the call site cannot see the target's C signature.
Spinel stamps that signature on the `Method` at its statically known bind site
(a legacy `sp_int` register ABI, or under `--int-overflow=promote` the boxed
poly ABI) and the poly path calls it through that stamp when the call fits
it. When it does not, the call takes the **thunk** the bind site synthesized
for the target: a per-target C function that reads the boxed arguments,
converts each to the parameter's C type, fills an omitted optional from its
default, packs a rest, calls the target with its real signature and boxes the
result. So a Float parameter or return, a call below full arity, a rest
parameter and a mixed promote signature all answer as CRuby does; a count the
signature cannot bind is CRuby's `ArgumentError`. What is left:

- The thunk converts at the boundary, by the parameter's compiled type. An
  argument of another kind -- a Float into a parameter the analyzer typed
  `Integer` because every visible call passed one, a String into a Float
  parameter -- raises `TypeError` (`wrong argument type Float (expected
  Integer)`) there, where CRuby would run the body with it (and usually fail
  inside it). A parameter whose type the analyzer could not see at all is
  `Integer` by default, so a method called ONLY through a Method object takes
  Integer arguments; give it one visible call with the intended kinds.
- A bound builtin's `__bam_` wrapper has no thunk: it keeps the stamped ABIs
  and declines with `NoMethodError` outside them
  (`[method(:puts)][0].call(1, 2)`). The proc ABI packs at most 16
  positional slots, so `Method#to_proc` of a target with more than 16
  positional parameters is not supported.
- A typed-array adapter value the typed array cannot hold (`arr.method(:push)`
  given a String, `arr.method(:[]=)` given a String value) raises the same
  `TypeError` every typed-array store raises for such a value (see "A typed
  array holds one kind of element" below). CRuby's Array is heterogeneous and
  would accept it; the typed array is what cannot. Through a poly slot
  (`[arr.method(:push)][0].call("z")`) the call is an ABI mismatch and
  declines with `NoMethodError` before any value is examined. An
  out-of-kind INDEX still raises CRuby's `TypeError`, and a zero-argument
  `arr.method(:[]).call()` raises `ArgumentError` rather than reading index 0.
  A zero-argument `arr.method(:[]).to_proc.call()` raises the same
  `ArgumentError` through the proc trampoline; the unmodeled two-argument
  slice form (`arr.method(:[]).to_proc.call(0, 2)`) still answers `arr[0]`
  where CRuby answers `[arr[0], arr[1]]`.
  The value refusals can mutate before they raise: `arr.method(:push).call(3,
  "z")` appends `3` and then refuses `"z"`, so a rescued
  `TypeError` leaves the array partially pushed (CRuby's untyped Array
  would have appended both).
- Over-arity is not modeled: a bound array operator whose wrapper/adapter C
  cast has a fixed operand count ignores operands past the ones it models.
  `ia.method(:[]).call(0, 2, 3)` answers `ia[0]` where CRuby raises
  `ArgumentError`, and `ia.method(:[]=).call(0, 9, 8)` sets index 0 where CRuby
  performs the slice assignment. The in-range slice forms (two arguments to
  `[]`, three to `[]=`) are accepted and ignored the same way. A multi-value
  `push` through a poly slot (`[ia.method(:push)][0].call(8, 9)`) declines with
  `NoMethodError` where the static route pushes both values.

A parameter default that WRITES a local the method body READS -- the
`def index_with(default = (no_default = true))` idiom, or
`def m(a, c = (z = a + 1; z)); z; end` -- is run by the callee rather than at
the call site, where the write could not reach the body: the parameter's
default becomes the private symbol `:__sp_absent`, the locals are declared
nil ahead of the body, and a guard binds the parameter from the original
default when it sees the symbol. The parameter's inferred type therefore
includes Symbol (a scalar parameter widens to a boxed one), and a caller
passing that very symbol is taken as omitting the argument.

A module method's optional parameter default is typed in the MODULE's own
scope, which cannot see the including class's instance-variable types. A
default reading such an ivar is therefore coerced into the parameter's
module-inferred slot: `module M; def m(a, b = @o); [a, b]; end; end` included
by a class with a String `@o` answers `[1, 0]`, not `[1, "hi"]`, and
`def m(a, b = @f + a)` with a Float `@f` answers `[1, 2]`, not `[1, 2.5]`.
The bound-Method routes box the field correctly; the loss is the parameter's
inferred Integer width, shared with the ordinary direct call.

A typed-array adapter Method (`arr.method(:push)`) reports the CRuby arity of
the Array op it stands in for (`-1`) except for `[]`, whose adapter Method still
reports `1` through a poly slot where CRuby answers `-1`.

#### Exception protocol from a genuinely poly value

An `Exception` subclass instance held in a genuinely poly value -- read out of
a heterogeneous container, or returned through a poly `Proc` -- dispatches
`#class` and `#inspect`, but `#message` raises `NoMethodError` and `#to_s` falls
back to `#<MyErr:0x...>` instead of the message, where CRuby answers the message
from both:

```ruby
class MyErr < StandardError; end
arr = [MyErr.new("boom"), 5]
e = arr[0]
e.class      # => MyErr          (as CRuby)
e.inspect    # => #<MyErr: boom> (as CRuby)
e.message    # => NoMethodError  (CRuby: "boom")
e.to_s       # => #<MyErr:0x...> (CRuby: "boom")
```

The class and message are carried on the boxed value, but the `#message`/`#to_s`
arm is only emitted for a receiver whose static type names the exception class.
The behavior predates and is independent of the bound-Method work above: it
reproduces through a poly `Proc` result as well.

### Sockets

`require "socket"` is mandatory (see [require.md](require.md)); without it the
constants are undefined, as in CRuby.

**Supported classes.** `TCPServer`, `TCPSocket`, `UDPSocket`, `UNIXServer`,
`UNIXSocket`, and the `BasicSocket` / `IPSocket` / `Socket` classes they inherit
from. They sit in CRuby's chain (`TCPServer < TCPSocket < IPSocket <
BasicSocket < IO`), so `#is_a?`, `#class`, `.superclass` and `.ancestors`
answer as CRuby does. (IO's own mixins, `File::Constants` and `Enumerable`, are
still missing, so `ancestors` diverges past `IO`.)

**Constructors.** `TCPServer.new(port)` / `TCPServer.new(host, port)`,
`TCPSocket.new(host, port)`, `UDPSocket.new`, `UNIXServer.new(path)`,
`UNIXSocket.new(path)`.

**Methods.** `#accept`, `#addr`, `#peeraddr`, `#local_address`,
`#remote_address`, `#bind`, `#connect`, `#send`, `#recv`, `#recvfrom`,
`#listen`, `#shutdown`, `#setsockopt`, `#getsockopt`, and
the whole IO surface a handle carries (`#gets`, `#read`, `#readpartial`,
`#write`, `#puts`, `#print`, `#flush`, `#close`, `#closed?`, `#eof?`,
`#fileno`, `#each_line`, the `IO#wait_*` readiness family, `IO.select`).
`#addr` / `#peeraddr` return CRuby's numeric 4-element form. `#accept` parks
cooperatively on the green-thread scheduler, so a thread-per-connection server
does not stall its siblings, and socket writes bypass stdio (`#sync` is true,
as in CRuby).

`Socket::` constants (`SOL_SOCKET`, `SO_REUSEADDR`, `AF_INET`, `SOCK_DGRAM`,
`SHUT_RDWR`, `TCP_NODELAY`, ...) resolve at run time from the system headers,
so they carry the right platform-specific values.

**Class methods.** `Socket.gethostname`, `Socket.getaddrinfo`, `Socket.pair` /
`.socketpair`, `Socket.new(domain, type, protocol)`,
`Socket.sockaddr_in(port, host)` / `.pack_sockaddr_in`,
`Socket.sockaddr_un(path)` / `.pack_sockaddr_un`, `Socket.unpack_sockaddr_in`.

**Addrinfo.** `Addrinfo.tcp` / `.udp` / `.ip` / `.unix`, and `#ip_address`,
`#ip_port`, `#afamily`, `#pfamily`, `#socktype`, `#protocol`, `#unix_path`,
`#ipv4?`, `#ipv6?`, `#unix?`, `#ip?`, `#to_sockaddr`, `#inspect`.
`#local_address` and `#remote_address` answer one.

**Socket::Option.** What `#getsockopt` returns: `#int`, `#bool`, `#level`,
`#optname`, `#family`, `#inspect`.

**Non-blocking.** `#accept_nonblock`, `#connect_nonblock`, `#recv_nonblock`,
`#read_nonblock`, `#write_nonblock`. Would-block raises the CRuby exception --
`IO::EAGAINWaitReadable` and friends, which answer to `IO::WaitReadable`, to
`Errno::EAGAIN` and to `SystemCallError` alike -- or, with `exception: false`,
returns the `:wait_readable` / `:wait_writable` marker. O_NONBLOCK is set for
the duration of one call and put back, so a blocking `#gets` on the same handle
still works.

**Divergences and gaps.**

- Only the **integer-valued** socket options are reachable, so
  `Socket::Option` carries an int rather than a byte string: `#data` and
  `#unpack` are missing, and `SO_LINGER` cannot be read back.
- `Addrinfo` covers the address itself, not the resolver surface:
  `Addrinfo.getaddrinfo`, `#getnameinfo`, `#canonname`, `#bind`, `#connect`,
  `#listen` are missing. `Socket.getaddrinfo` returns CRuby's
  array-of-arrays form, which is the usual way in.
- `#recvfrom_nonblock`, `#sendmsg` and `#recvmsg` are missing; the rest of
  the non-blocking family (`#accept_nonblock`, `#connect_nonblock`,
  `#recv_nonblock`, `#read_nonblock`, `#write_nonblock`, and their
  `exception: false` forms) is supported.
- `SOCKSSocket` does not exist (CRuby only defines it when built with the
  SOCKS library, so a program cannot rely on it either).
- Missing instance methods: `#getsockname`, `#getpeername`,
  `#do_not_reverse_lookup`.
- Missing class methods: `.open`, `.gethostbyname`, `IPSocket.getaddress`.
- `TCPServer.new` takes no backlog argument (the listen backlog is fixed);
  `TCPSocket.new` has no four-argument local-address form.
- A class Spinel recognizes but has not implemented reports the missing
  **method** (`undefined method 'new' for class ...`), not a missing constant.

---

## By design (deliberate choices)

- **Integer overflow** -- pick one mode at compile time: `raise` (default,
  `RangeError` on overflow), `wrap`, or `--int-overflow=promote` (auto-bignum).
  Not both in one binary, because the representation is chosen statically. See
  [int-overflow.md](int-overflow.md).
- **Float `round(ndigits)`** -- the value is always correct; the *return class*
  follows CRuby (Integer for `round` with 0 digits, Float otherwise).
- **`Proc#ruby2_keywords`** -- not supported (rejected at compile time). It is a
  migration shim for the Ruby 2.x-to-3.0 keyword-argument transition, flagging a
  proc so a trailing `Hash` forwarded through `*args` is treated as keywords.
  Spinel targets modern Ruby keyword semantics directly, so the shim has nothing
  to toggle; there is no 2.x behavior to opt back into.
  `Hash.ruby2_keywords_hash` / `Hash.ruby2_keywords_hash?` are the same shim
  from the hash side (marking / reading the flag) and are rejected the same
  way.
- **A bundled library carries only what it uses.** A `require` loads that
  library and nothing else. CRuby's own stdlib files often pull in others as an
  implementation detail -- `require "csv"` loads `stringio` there, so a program
  that requires csv can name `StringIO` without requiring it -- and Spinel's
  version of the same library has no reason to make the same internal choice.
  A program must `require` what it actually uses; the transitive requires of
  CRuby's implementation are not part of a library's interface. (Spinel's
  bundled libraries are listed in [require.md](require.md).)
- **`slice_before` / `slice_after` with a `Proc` pattern** -- rejected at
  compile time (a stored-proc `===` call per element); use the block form.
  Range, Class, Regexp, and value patterns are supported.
- **`Comparable` with a non-conforming `#<=>`** -- `<=>` is a protocol method
  returning `Integer` or `nil` (a `Float` is accepted, compared by sign). A
  `<=>` whose result type is statically something else (`String`, `Array`,
  `Hash`, `Symbol`, boolean) is a definite protocol violation: any Comparable
  operator (`<`, `<=`, `>`, `>=`, `==`, `between?`, `clamp`) on such a receiver
  is rejected at compile time rather than raising at run time as CRuby does.
  A `<=>` whose result is only `poly`/unknown statically keeps the CRuby
  runtime behavior (an incomparable pair raises `ArgumentError`).
- **`remove_method` / `undef_method` / `remove_class_variable`** -- rejected at
  compile time. Methods are resolved statically and compiled to direct C calls,
  and class variables to static storage, so there is no runtime table for these
  to mutate; a construct would remove nothing. The call is reported rather than
  silently ignored. (A class that defines its own method by one of these names
  keeps it.)
- **Frozen literals** -- explicit `.freeze` then mutation raises `FrozenError`,
  matching CRuby. String literals ARE frozen by default here
  (`frozen_string_literal: true` semantics, with no opt-out) -- see
  "String literals are frozen by default" below for what that changes and
  where mutable strings come from.
- **`nil` meeting a String is a nullable String, not untyped.** Where a
  value can be either nil or a String -- a ternary, an `if` with no else, a
  `return nil if ...` ahead of a String, a `case` with no matching arm, a
  local written nil on one path and a String on another, a `next nil` in a
  block -- the slot stays a String whose C form carries nil as NULL, the
  same representation `v&.upcase`, an ivar written nil and a `String?` seed
  already use, and every String consumer reads it as nil (`nil?`, truth,
  `to_s`, interpolation, boxing, and `NoMethodError` from the rest). It
  used to widen to untyped, and `return nil if v.nil?` at the head of a
  method was the largest single source of the boxed slow path in a real
  tree. A literal `nil` inside an array or hash literal keeps the
  container boxed, as before. `nil` meeting an Integer, Float or bool
  still widens to untyped: the nullable Integer and Float slots that exist
  (an ivar written nil, an `Integer?` seed) carry a sentinel, and a
  comparison on one raises as CRuby does (`nil > 0` is NoMethodError,
  `1 > nil` the Comparable ArgumentError, `nil <=> 1` nil), as does one
  reaching a strict Integer argument -- an index, a count, a width --
  where `s[s.index("z")]` is the same `no implicit conversion from nil to
  integer` CRuby raises rather than a read off the front of the string.
  A Range endpoint is the exception it is in CRuby: `s[ix..]` on a missed
  `index` is the beginless Range, not an error. But bool and Symbol have
  no spare value at all.
- **Comparable is keyed on `<=>` presence** -- the Comparable operator methods
  (`<`, `<=`, `>`, `>=`, `between?`, `clamp`) work on any class that defines
  `<=>`; CRuby additionally requires `include Comparable` (a `NoMethodError`
  otherwise). Spinel does not model the mixin, so it is permissive where CRuby
  raises. `sort`/`min`/`max`/`minmax` need only `<=>` in both. Related edges:
  the comparison-failed message names an operand's class where CRuby inspects
  special constants (`NilClass` vs `nil`); `sort_by` keeps incomparable keys
  in their original order where CRuby raises; `include?`/`index` on arrays of
  user objects compare by identity unless the class defines its own `==`.
  Sorts run a deterministic stable merge (identical on every platform, unlike
  libc `qsort`); it matches CRuby's comparison schedule for small arrays, but
  for larger ones (roughly 8 elements and up, where CRuby switches to its
  quicksort) the order of tied elements and which incomparable pair the
  ArgumentError names can differ from CRuby -- deterministically so.
- **Thread data races are observable** -- Spinel runs threads with real
  parallelism and no GVL, so two threads mutating the same `Array`/`Hash`/object
  without a `Mutex` race, similar to `Array`/`Hash` in JRuby and `Array` in
  TruffleRuby. What that costs differs by kind of state, and `docs/thread.md`
  says which: an object never loses an ivar and a word-sized ivar never tears,
  a multi-word ivar (`Range`, `Time`, `Complex`, `Rational`) can be read half
  from one write and half from another, and a shared `Array`/`Hash` can abort
  or SIGSEGV.
  CRuby's GVL makes individual operations appear atomic; Spinel does
  not, and adds no implicit per-object locking -- correctness across threads is the
  program's responsibility via `Mutex`/`Queue`/`ConditionVariable`. Relatedly,
  thread *interleaving* (and so the ordering of `Thread.pass`, `Thread.list`
  membership, and the exact moment a `Thread#raise`/`#kill` is delivered) is
  nondeterministic, where the single-worker model was deterministic.
  `Thread#raise`/`#kill` targeting the **main** thread is a no-op (main runs on
  the scheduler's root fiber, which has no inject delivery points); CRuby
  delivers the exception to main.

### Intentional incompatibilities with CRuby

Spinel aims to be a subset of Ruby: programs it accepts should behave the same
as on CRuby. In a few cases CRuby's behavior depends on a feature Spinel does
not implement, and silently returning a wrong value would be worse than a
visible error. Those deliberate divergences are listed here.

#### Two DISTINCT self-containing Sets compare equal

A container walk that meets an object it is already inside stops there and
calls the pair equal, which is what makes `a = []; a << a; a == a` terminate
at all. For Arrays, Hashes, Structs and plain objects that agrees with CRuby.
For two *distinct* Sets that each contain themselves it does not: Spinel
answers `true` where CRuby answers `false`, and everything reaching `==` or
`eql?` follows -- `include?`, `subset?`, `superset?`, `intersect?` answer
true, `disjoint?` false, `<=>` 0 rather than nil, and a Hash keyed by one
finds the other, as do `Array#uniq`, `Array#-` and `Array#include?`.

CRuby's Set is a hash table, and an element's hash is stored when it is added
-- while that Set is still empty -- so its later membership probe misses.
Spinel's Set is Array-backed with a linear `eql?` scan and has no stored
per-element hash to miss with. The same Set compared with itself, and every
non-recursive Set, agree with CRuby.

#### `Set::RecursionGuard`

The Set package reaches the runtime's recursion path through bindings in a
nested `Set::RecursionGuard` module, so `Set.constants` lists it and
`Set::RecursionGuard.respond_to?(:enter_eq)` is true where CRuby raises
NameError. It is not an API and nothing else about Set's surface changes;
`Set.constants` already differed from CRuby's, which lists `CoreSet`.

#### A `super` chain whose callers pass differently-typed blocks

A method that yields is inlined at every call site, so it is specialized to
the block it is given there. A `super` reaching such a parent carries the
child's own caller block down, and the chain is inlined the same way -- a
three-link chain, several yields under the super, a class-method `super` and
`super(args)` all work.

What does not is a chain of three or more links where two callers pass blocks
whose *values* have different types (one returning an Integer, another a
String) and both routes meet at the same yielding ancestor: the middle link
carries a single type, so one of the two sites gets the wrong one and the
generated C is rejected. Two links are fine -- the parent is specialized per
call site there. Give the ancestor its own parameter, or make the block values
agree, if a chain that deep needs both.

#### A `nil` read out of an Integer container

A missing key on an Integer-valued Hash, or an out-of-range index on an
Integer array, answers `nil`. Spinel represents that `nil` as a sentinel
value inside the int slot, so the value is `nil` for `nil?`, `inspect`,
`class` and `||`, and every consumer that can see it raises the way CRuby's
`nil` does: arithmetic (`+`, `-`, `*`, `/`, `%`, `abs`) is `NoMethodError`
or the coercion `TypeError`, comparisons and the numeric predicates
(`<`, `>`, `<=>`, `zero?`, `positive?`, ...) are `NoMethodError` or the
Comparable `ArgumentError` (#4567), and a strict Integer argument -- an
index, a count, a width -- is `no implicit conversion from nil to integer`
(#4896). The test is emitted only where the analysis says the value can
carry the sentinel, so a loop counting from a literal keeps its bare
compare and its bare index.

What is left is the sentinel reaching a slot through a shape the analysis
does not mark: a `Range` VALUE built from one (`r = (h[k]..); s[r]`) still
slices from the raw sentinel rather than reading as the beginless Range a
literal `s[h[k]..]` does. A genuine `-9223372036854775808` stored in such a
slot is indistinguishable from nil, which is the price of the
representation.

#### `Integer#**` with a negative exponent

CRuby evaluates a negative integer exponent to a `Rational`. Spinel matches
it whenever the sign is knowable: a literal negative exponent types the
result `Rational` statically (`2 ** -1 # => (1/2)`, `0 ** -1` raises
`ZeroDivisionError` as in CRuby), and the poly-dispatched path (a
poly-typed base or exponent, e.g. promote-mode parameters) picks `Integer`
or `Rational` from the sign at run time. The residual divergence is a
statically int-typed runtime exponent (`x ** y` with plain int locals):
typing it a sometimes-`Rational` would force the result poly and cascade
through every int-arithmetic consumer, so a negative value there still
raises `RangeError` rather than silently truncating. `Integer#pow(negative,
mod)` raises `RangeError` with CRuby's message.

#### `Integer#**` / `Rational#**` with a `Rational` exponent

CRuby returns an exact `Rational` when the exponent is integer-valued
(`3 ** 2r # => (9/1)`, `Rational(3,4) ** 2r # => (9/16)`) and a `Float`
otherwise. Spinel returns a `Float` in every case (`3 ** 2r # => 9.0`): the
exactness depends on the exponent's denominator at run time, so honoring it
would force the result to a boxed union and cascade through consumers. The
exponent's magnitude is still correct; only the class (Float vs Rational)
differs. `Integer ** Complex` and a `Complex` exponent generally evaluate to
the correct `Complex`, and `Integer#fdiv` / `#div` with a `Rational` argument
are exact.

#### A `Range` object needs `Integer`/`Float`/`String` bounds

A `Range` is an unboxed value with `sp_int` bounds, so a Range **object** over
user objects cannot be built (`rng = (Ver.new(1)..Ver.new(9))` is a compile
error naming the class). Comparing against such a range does not need one:
`Comparable#clamp` folds the bounds straight into the comparison, so the inline
and one-sided forms work.

```ruby
x.clamp(lo..hi)     # works -- no Range is built
x.clamp(lo, hi)     # works
x.clamp(..hi)       # works (one-sided)
x.clamp(lo..)       # works
rng = (lo..hi)      # compile error: a Range of Ver objects cannot be built
rng = (0..2**70)    # compile error: a Bignum bound does not fit sp_int
```

#### A call that cannot exist is refused at compile time, not raised at run time

Spinel resolves what it can resolve at compile time -- that is the point of the
AOT model -- and an undefined method is no exception. Where the receiver's type
is known and neither the class nor CRuby's own surface for it carries the name,
the call cannot succeed under any input, so it is reported when the program is
built rather than left to raise:

```ruby
[1].nope        # spinel: t.rb:1: undefined method 'nope' for an instance of Array (NoMethodError)
:s.nope         # ... for an instance of Symbol
Plain.new.nope  # ... for an instance of Plain
```

The diagnostic is CRuby's own wording, so the message reads the same as the
exception would; only the moment differs. The consequence is that a program
which *only reaches such a call behind a `rescue NoMethodError`* cannot be
built:

```ruby
r = (begin; [1].nope; rescue NoMethodError => e; e.receiver; end)   # compile error
```

A receiver whose type is not statically known -- a `nil`, a boxed value read
out of a container, a poly union -- keeps the runtime raise, since nothing
could be proved about it at compile time:

```ruby
x = nil
r = (begin; x.nope; rescue NoMethodError; "runtime"; end)   # => "runtime"
```

A name CRuby *does* define on that class, which Spinel has not implemented, is
a different thing entirely: that is a gap in Spinel, and it reports itself as
an `unsupported call` naming the node, not as a `NoMethodError`.

#### A typed array holds one kind of element

An Array whose every visible element is an Integer, a Float or a String is
compiled as a typed array (`sp_IntArray`, `sp_FloatArray`, `sp_StrArray`),
which is what makes numeric code fast. A store the compiler can see both
sides of widens the array instead (`a = Array.new(0, 0); a << "x"` makes `a` a
general Array), so the typed representation is only kept where every store
agrees. A value whose kind is decided at run time -- an element read out of a
general Array, a boxed parameter, a poly-typed call -- that does not match
the array's kind cannot be stored, and raises `TypeError` at the store:

```ruby
def collect(out, src)
  i = 0
  while i < src.length
    out << src[i]      # src[i] is decided at run time
    i += 1
  end
end
a = Array.new(0, 0)
collect(a, [1, "z"])   # TypeError: cannot store String into an Array[Integer]: a typed array holds one kind of element
```

CRuby's Array would hold the String. Spinel used to coerce instead (`"z".to_i`
into an Integer array stored `0`, an Integer into a String array stored `""`),
which was a wrong value said nothing about; refusing is the one answer that
never lies about what the array holds. `nil` is the kind's own nil and is
stored; an Integer stored into a Float array is promoted, as CRuby's
arithmetic would promote it. Every store route answers the same way: `<<`,
`push`, `unshift`, `insert`, `concat`, `fill`, `[]=`, the runtime dispatch on
a boxed array, and a typed-array `Method` adapter. A nested store through a
general container (`grid[r][c] = v` where the row is typed) is the one route
that widens the row to a general Array instead, since the container is what
holds it.

#### A typed array the compiler cannot follow is not copied into a parameter the method mutates

A parameter the method stores elements of another kind into is compiled as a
general Array (or a boxed value, when its callers disagree), and a typed
array (`Array[Integer]`) passed to it has to be one too, since the two store
their elements differently. The compiler follows the argument back to where
its arrays are built and builds them as general Arrays: an array literal or a
new array, through locals (their `||=` and the arms of a branch too), the
value of a method (or a chain of them, a method a subclass overrides, or a
method answering its block's value), a `then` block, a multiple assignment
from a literal, a row of a literal table, a builtin that answers its receiver
(`push`, `concat`, `tap`), an ivar or attr_reader every write of which builds
a new array or keeps a parameter of the method writing it, a global, class
variable or constant. The method's other callers then read the general Array
too. A boxed argument -- a local written arrays of two kinds, an element read
out of a general container, a block parameter, a `for` variable -- is
followed the same way, to each typed array the store cannot fit.

Where it cannot follow a typed argument, the one conversion left is a copy,
and the mutation would land in the copy where CRuby changes the caller's
array. Such a call is refused at compile time:

```ruby
Box = Struct.new(:a)            # the member keeps the caller's array
def add(out) = out << "z"
src = [1, 2]
add(Box.new(src).a)
# spinel: t.rb:4: an Array[Integer] is passed to `add`'s parameter `out`, which the method mutates: ...
```

What it does not follow: a Struct member, or an ivar an attr_writer writes,
that keeps an array handed in from outside; an array an rbs seed declares
typed (a parameter or a return); a call through a receiver of no one class;
the value of a lambda or proc; a new array (`Array.new(2, 0)`) a multiple
assignment binds. Build the array as a general Array where it is created, or
give the parameter the argument's kind (an rbs seed, or call sites that all
pass the same kind).

#### A plain String in a slot that holds other values too is not shared by `<<`

A local, ivar, Hash value or Array element that holds more than one kind of
value can carry a String two ways. As a shared handle (where a String in it
is changed in place elsewhere in the program) it behaves as in CRuby. As a
plain boxed String, `<<` builds the longer String instead of changing the
old one. A `<<` (or a chain of them) on a local, or on an ivar inside a
method, stores the result back into that slot, whether its value is used or
not, so the slot is right; but another name for the same String does not see
the change. (An ivar a class body writes, outside any method, does not take
the result back at all yet.)

```ruby
h = {}
[1].each { |i| h[i] = "v#{i}" }
h[2] = 5          # the values are now Strings and Integers
o = +h[1]
o << "+"
p o               # "v1+" in both
p h               # {1 => "v1+", 2 => 5} in CRuby, {1 => "v1", 2 => 5} in Spinel
```

#### A String a method appends to is not yet shared through some dynamic calls

A method that appends to its String parameter (`s << x`, `concat`,
`insert`, a `!` method) changes the caller's String in CRuby, since both
names hold the one object. Spinel shares the String by reference through a
direct call, `send` with a literal name, `super`, a poly receiver and a
class value; through a proc, a lambda and a `Method` (`.call`, `.()`,
`[]`, `===` and `.yield` on one, a block kept as `&blk` and called later,
`method(:m)` and `obj.method(:m)` and their `to_proc`, whether the String
is passed by position or by keyword); through `yield`, into a literal
block, a block the method keeps, and a proc or `Method` passed with `&`;
through `instance_exec`, `instance_eval`, `class_exec` and `module_exec`;
through `new` and `raise Cls, s` into an `initialize`, one that yields the
String to its block, a Struct's and a Data's included; and through a splat
or a gather, into any of those (`m(*args, s)`, `m(*[s])`, `C.new(*[s, s])`,
`def m(*r)`). The paths below do not share it yet, and a call that would
hand such a method a String variable through one of them is refused at
compile time rather than compiled with the append lost:

```ruby
f = ->(t) { t << "!" }
["a", "b"].map(&:dup).each { |s| f.call(s) }
# spinel: t.rb:2: a String is passed to a proc's parameter `t` through `f.call`, which the proc appends to: ...
```

It is shared as well through an UnboundMethod (`bind_call`,
`instance_method(:m).bind(o).call`), a method `define_method` defines, a
curried proc, and a proc or `Method` read out of a slot that holds other
values too.

Not yet shared:

- a String variable in a splatted Hash literal (`**{ k: v }`) at a dynamic call or yield whose key binds an appending keyword parameter;

- a String variable in an Array literal feeding an appended nested multiple-assignment target;

- a bare instance-variable argument written from a local, handed to an appending parameter through a call or `super`, unless the instance variable is already a shared handle;

- a repeated keyword whose later value is a String variable bound to an appending parameter, unless the value is already passed as a shared handle;

- through `Thread.new` or `Fiber#resume`, a String variable handed to a block parameter that appends to it, unless its read already hands over the shared handle or the local is read only as that argument;
- through a Hash's value block (`each_value`, `each`, `each_pair`, or an element iterator over `values`), a stored String variable when the value parameter appends to it;
- through `yield` into a capture-wrapper block, a String variable whose captured parameter appends to it without already being the shared handle, including a splatted yield;
- through an Array's chained index into an appending block;

- through a retained `scrub!` result that is appended to; `scrub!` with a block is also refused because the block would be ignored;

- through an ivar's or a call's Array, a fresh Array literal, a narrowed boxed String element, or a fresh String's `tap`, into an appending block or parameter;

- by keyword, through a curried proc;
- through `instance_exec`, a String variable in or ahead of a splat
  (`o.instance_exec(s, *rest) { |t, *r| t << "!" }`), and one held by a
  block parameter, by a variable a proc captures, or by a global or class
  variable; through a `yield` into a block the method keeps or a proc
  passed with `&`, one held by a block parameter or by a global or class
  variable;
- through a splat of a local Array the program changes after its literal,
  a String it holds that is no shared handle: a global or an instance
  variable pushed into it, or the contents of another Array
  (`s.replace(t)`);
- through a proc, a `Method` (bound, unbound, or read out of a slot), a
  curried proc, a method `define_method` defines, `new` or `raise`, a
  String held by a block parameter, by a variable a block or proc captures,
  or by a global or class variable, and through a proc, a `Method` or a
  class value's `new`, one held by an instance variable.

A String is shared as well through a rest a method forwards (`def w(*a) =
m(*a)`, `def w(*) = m(*)`, `def w(...) = m(...)`, `def m(*) = super`) and
through a parameter a method hands on to `super` or a call after a `**h`
call typed it POLY; through those, one held by a block parameter, a
variable a proc captures, an instance variable that is no shared String,
or a global or class variable is refused. A String variable in or past a
splat into `super` or `yield` (`super(*s)`, `yield(*e, v)`, a rest yielded
as `yield(*r)`), bound to a parameter that appends, is refused as well, and
so is any String forwarded to the 17th position or past it, or handed on
through more POLY parameters than the analysis follows.

Each is lifted in turn, and this list shrinks with it. Until then, return
the String from the method and assign it, or append to it in the caller. A
literal or any other expression passed there is not refused: nothing else
can see its growth.

#### A reassigned block parameter, and `yield` inside a proc literal

Assigning to a method's `&block` parameter used to be refused where it was
written. It now compiles as the rewrite that used to be asked for, done by
the compiler: the parameter's value moves to a fresh local at the top of the
body, the writes and later reads use that local, and `yield` and
`block_given?` keep seeing the block the caller passed, which is CRuby's
split too.

```ruby
def fetch(key, &blk)
  blk ||= proc { |k| raise KeyError, k }   # works
  blk.call(key)
end

def dispatch(*args, &block)                 # activesupport's BroadcastLogger
  if block_given?
    called, result = false, nil
    block = proc { |*a| called ? result : (called = true; result = yield(*a)) }
  end
  targets.map { |t| t.send(:log, *args, &block) }
end
```

A `yield` inside a `proc` or `lambda` literal is the other half of that
shape. The literal is a real closure, its own C function, with no inlined
caller's block to reach, so the yield raised LocalJumpError at run time. It
now calls the method's block parameter as a value, `blk.call(args)`, and a
def with no named block parameter is given `&__blk` for the purpose. A
block attached to any other call (`each { yield }`) is inlined with its
method and keeps its `yield`.

A method whose block use is `block_given?` plus a value use of the parameter
(`x = blk`, a capture in a proc, a `&blk` forward) takes the same lowering a
method with a literal `yield` and a value use does: it is emitted as one
function with the block as a real parameter rather than inlined per call
site.

#### A nested numeric table or an object array is boxed by reference

`Array[Array[Integer]]`, `Array[Array[Float]]` and an Array of one class's
objects are compiled to an unboxed pointer array when every use supports it
(see docs/rbs-extract.md). Reaching a slot that holds any kind of value -- a
method whose value is the table on one path and `nil` on another, an element
of a general Array, a Hash value, a boxed parameter -- boxes the array by
reference, stamped with what its elements are, so the boxed value is the same
array (a mutation through either side is seen by both) and reads, `inspect`,
`==`, iteration and the rest answer as they would for a general Array:

```ruby
def run(flag)
  run_bcf if flag      # the table on one path, nil on the other
end
p run(true)            # [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]]
```

The typed-array rule above applies through the box: a store of another kind
(`t << "x"`, `t.push(1)`, `t.insert(0, :s)`, `t.concat(["q"])`) raises
`TypeError`, where CRuby's Array would take the element. An object array of
one class takes that class and its subclasses.

#### A `Float::INFINITY` bound reports the other bound as a `Float`

An integer `Range` is a value with `sp_int` bounds, which have no
representation for an infinity: the value can only record "unbounded". Where
that loses information CRuby keeps, Spinel resolves it as follows.

A range whose **begin** is infinite (`-Float::INFINITY..5`) takes the `Float`
representation, so `#begin` answers `-Infinity` as CRuby does. Its finite end
then reports as a `Float`:

```ruby
(-Float::INFINITY..5).begin    # => -Infinity   (as CRuby)
(-Float::INFINITY..5).cover?(0) # => true       (as CRuby)
(-Float::INFINITY..5).end      # => 5.0         (CRuby: 5)
(-Float::INFINITY..5).to_s     # => "-Infinity..5.0"  (CRuby: "-Infinity..5")
```

A range whose **end** is infinite (`1..Float::INFINITY`) keeps the integer
representation -- it is the canonical lazy source, and its integer enumeration
is what a fused `.lazy` pipeline walks. `#end`, `#size` and `#to_s` read the
bound off the literal, so they answer as CRuby does; a range of that shape held
in a variable and asked for `#end` answers `nil` (the value records only that
it is unbounded):

```ruby
(1..Float::INFINITY).end     # => Infinity      (as CRuby)
(1..Float::INFINITY).size    # => Infinity      (as CRuby)
(1..Float::INFINITY).to_s    # => "1..Infinity" (as CRuby)
r = (1..Float::INFINITY); r.end   # => nil      (CRuby: Infinity)
```

A finite mixed range (`1..5.0`) keeps the integer representation, where its
`#to_a`, `#sum` and `#cover?` are all right and its iteration is the integer
one CRuby performs; only `#end` reports `5` where CRuby reports `5.0`. A
one-sided float range (`(..5.0)`, `(1.0..)`) likewise keeps it, so `#to_s`
renders the bound as an integer (`"..5"`). A range literal that is only
matched (`when ..2.5`, `(1.5..) === x`, `.cover?(x)`) takes the `Float`
representation instead, so it compares its bound as written.

A `String`-bounded range (`("a".."e")`) is its own value type, so it keeps its
class, `#to_s` and `#inspect` whether it is used inline or held in a variable.
Its endpoint and membership methods (`begin`/`end`/`min`/`max`/`cover?`/`===`)
answer directly; every traversal (`each`, `map`, `to_a`, ...) materializes the
element array through `String#succ`, so an unbounded string range cannot be
iterated. `#size` is `nil`, as in CRuby, since a string range has no integer
element count.

#### `Time` sub-second precision is nanoseconds

A `Time` value stores its sub-second as an integer nanosecond count
(`int32 tv_nsec`), like `struct timespec`. A `Rational` sub-second argument
that does not fall on a nanosecond boundary is rounded to the nearest
nanosecond at construction:

```ruby
t = Time.utc(2020, 1, 1, 0, 0, 0, Rational(1, 3))  # 1/3 microsecond
t.subsec    # => (333/1000000000)   (CRuby: (1/3000000))
p t         # => 2020-01-01 00:00:00.000000333 UTC
            #    (CRuby: 2020-01-01 00:00:00 1/3000000 UTC)
```

Everything representable in whole nanoseconds -- every Integer `usec`, and
any `Rational` whose value lands on a nanosecond -- is exact, and `to_s`,
`usec`, `nsec` and `strftime("%N")` agree with CRuby. Only sub-nanosecond
exactness (and, as its consequence, the `Rational`-form `#inspect` display
CRuby uses for non-decimal sub-seconds) is lost.

#### Unboxed value types: identity IS the value

`Complex`, `Rational`, and `Range` values are unboxed C structs with no
internal pointers, so there is no per-object address to observe: `equal?`
(and `object_id` comparisons) are component equality. `x.equal?(x)` is `true`
as in CRuby, but two separately-constructed equal values also compare
`equal?` (CRuby: `false`). This extends the treatment CRuby itself applies to
its immediate values -- `1.equal?(1)`, `:s.equal?(:s)`, and (on 64-bit)
`1.0.equal?(1.0)` are all `true` there because the value is the identity.
The same applies to `freeze` on these values: they are value-frozen already
(`frozen?` is `true`), and `freeze` is an identity no-op.

**`/i` folds one codepoint to one.** Case-insensitive matching uses Unicode
simple case folding, so `/ä/i` matches "Ä" and `/k/i` matches "K" (U+212A).
A source whose fold is several codepoints has no single counterpart to fold
to and is matched literally: `"ß" =~ /ss/i` is `nil` where CRuby answers `0`.
Building the regexp engine with `-DRE_NO_UNICODE_CASE` or
`-DRE_NO_UNICODE_CTYPE` (`make RE_CASE_FLAGS=-DRE_NO_UNICODE_CASE`) is mruby's
`MRB_USE_ASCII_CTYPE` build: it leaves the case tables and the type table out
together, folds ASCII alone, and a non-ASCII literal then matches literally
under `/i` too.

**A pattern may have at most 31 capture groups.** The match registers `$~`
and `$1`..`$9` are built from hold that many, and so does the frame that saves
them across a call to a method that matches, so a wider pattern is refused
with `too many capture groups (maximum 31)` rather than compiled and then
truncated to what fits. CRuby has no ceiling here. 31 is where the registers
sit rather than where a program is likely to need to stop: of the 8,135 regexp
literals in CRuby 4.0.4's stdlib and bundled gems, the widest has 8 capture
groups. `(?:...)` costs nothing against it, and a named group counts as one.
The refusal is at compile time for a literal and at run time for a pattern
built there.

**A regexp literal is compiled when the program is.** A pattern the engine
cannot read is refused at compile time rather than raising `RegexpError` when
the built program reaches the literal, which is where CRuby reports it too
(as a `SyntaxError` from the parse). A pattern built at run time -- an
interpolated literal, `Regexp.new` on anything but a constant -- is still a
runtime question and still raises `RegexpError`.

**A search that backtracks is bounded by the state it holds.** A pattern with
a backreference, a lookaround or an atomic group runs on the backtracking
engine, whose choice points and undo records are capped together by
`MRB_REGEXP_STACK_LIMIT` (32768 entries). A greedy repetition leaves one
choice point per iteration, so what a search holds grows with the length of
the subject: `"a" * n + "b" + "a" * n =~ /\A(a+)b\1\z/` is answered for n up
to roughly 30000 and gives up above it, where CRuby keeps going. Giving up
raises `RegexpError` (`stack limit over (MRB_REGEXP_STACK_LIMIT)`) rather than
answering `nil`: a search stopped at the limit has not shown that there is no
match, and a `nil` there would be a wrong answer whenever the match lay past
it. The step ceiling (`MRB_REGEXP_STEP_LIMIT`) bounds the catastrophic shapes
the same way, so `("a" * 40 + "!") =~ /(a*)*b\1/` raises in milliseconds
rather than running for years.

**A POSIX bracket and a word boundary read Unicode above ASCII.**
`[[:alpha:]]` and its ten siblings hold what CRuby's brackets hold in every
script, and `\b` / `\B` sit beside a character of any script, both read off
the type table in `lib/regexp/re_ctype.h`. `\d`, `\w` and `\s` are ASCII in
Ruby's syntax and stay so, exactly as in CRuby, so `/\w/` and `/\b/` answer
different questions about the same character on purpose. The ASCII build
(see the fold note above) leaves the type table out, and a bracket then holds
its ASCII set alone, which the boundary reads too.

**Unicode properties are the POSIX names, the general categories and the
emoji properties.** `\p{name}` holds the characters with the property, and
`\P{name}` or `\p{^name}` the ones without it, inside a class or outside one.
The POSIX names (`Alpha`, `Alnum`, `Word`, `Space`, `Upper`, `Lower`, `Digit`,
`Punct`, `Graph`, `Print`, `Blank`, `Cntrl`, `XDigit`, `ASCII`) are the brackets
under another spelling, `/i` included, except that `\p{Punct}` leaves out the
nine ASCII symbols `$`, `+`, `<`, `=`, `>`, `^`, `` ` ``, `|` and `~` that
`[[:punct:]]` holds, as CRuby does. The
general categories are the thirty two-letter ones (`\p{Lu}` .. `\p{Cn}`) and
the first letter alone for a group (`\p{L}`, `\p{M}`, `\p{N}`, `\p{P}`,
`\p{S}`, `\p{Z}`, `\p{C}`); the emoji properties are `Emoji`,
`Emoji_Presentation` and `Extended_Pictographic`. Names match as CRuby
matches them: case, `_`, `-` and spaces make no difference. Under `/i` a
property folds as the class of its members would (`/\p{Lu}/i` holds "e"), and
a negated one follows CRuby: `\P{Lu}` under `/i` holds no cased letter, while
`[\P{Lu}]` holds them all. A script (`\p{Han}`), a binary property
(`\p{Alphabetic}`), an age, a block and every other name raise `RegexpError`
naming the property, and a property cannot end a range (`[a-\p{L}]`). The
tables come from the Unicode 17.0.0 database (`lib/regexp/re_prop.h`, about
18KB); the ASCII build (see the fold note above) leaves them out, answers the
POSIX names from ASCII as its brackets do, and refuses the categories and the
emoji properties rather than answer them from ASCII.

**A regexp construct the engine does not carry is refused, not read as its
letters.** `\K` (drop what was matched before it), `\R` (any linebreak) and
`\X` (a grapheme cluster) each mean something in CRuby that this engine does
not do. Left as unknown escapes each was simply its own letter, so `/\R/`
matched an `R` rather than a newline. They raise `RegexpError` at compile time
instead. Inside a character class CRuby reads `\K`, `\R` and `\X` as the
letter too, and so does the class parser here, so `[\R]` still matches an `R`.
`\G` and `\g<name>` ARE carried and behave as CRuby does.

The same applies inside a character class, where a `[` never stands for
itself: `[[.a.]]` (a collating element) and `[[=a=]]` (an equivalence class)
each raise `RegexpError` rather than compile a different pattern than the one
written. A nested class is read as CRuby reads it, the union of its members
(`[[a][b]]` is `[ab]`), and so are `[[:alpha:]]` and `&&`; `[\[]` holds the
bracket itself as it does in CRuby.

**A byte escape that starts no character is that byte.** The engine has no
encodings: `/[\x80]/` matches the byte 0x80, where CRuby refuses a UTF-8
pattern with `invalid multibyte escape`. It reads the same byte inside a
nested class or a `&&` intersection, where CRuby answers differently again.

**An `--rbs` seed is enforced where a value crosses into it.** A parameter
seeded `Hash[Symbol, untyped]` handed a hash whose keys the caller widened to
any type converts at the call, and a key the declared type cannot hold raises
`TypeError` there. CRuby ignores the signature, so a program whose keys really
are Symbols agrees with it and one whose keys are not diverges: the seed is a
claim about the program, and this is where the claim is checked.

**Regexp literals share one compiled object.** Each pattern is compiled once
at startup and every textually-equal literal names that one object, so
`/ab/.equal?(/ab/)` is `true` (CRuby allocates per literal: `false`). Same
shared-immutable-storage treatment as above; `==`/`eql?`/matching are
unaffected.

**String literals are frozen by default (`frozen_string_literal: true`
semantics).** Spinel's baseline is the direction Ruby itself is headed
(plain CRuby already warns "literal string will be frozen in the future"):
a literal is frozen (`"lit".frozen?` is `true`), mutating one raises
FrozenError, and mutable strings come from `+"lit"`, `String.new`,
interpolation, or `dup` -- exactly as under CRuby's
`--enable=frozen-string-literal`. There is no opt-out: a
`# frozen_string_literal: false` magic comment warns at compile time and
is ignored, and `--disable=frozen-string-literal` is rejected. (The
whole-program shared-mutable-string machinery relies on the frozen-literal
guarantee; a chilled mode would be a second, subtly different mutation
semantics.)

A note on identity: what `frozen_string_literal` specifies is frozenness,
not object identity, so the identity of frozen literals is
implementation-dependent. CRuby happens to intern equal-content literals
(`"abc".equal?("abc")` is `true` there); Spinel compiles each literal
OCCURRENCE to its own static object, so the same comparison answers
`false`, while re-evaluating one occurrence (a literal in a loop) yields
the same object where plain CRuby would allocate per evaluation. Programs
should not depend on either arrangement -- `equal?`/`object_id` on frozen
literals is exactly the implementation-defined corner. Value semantics
(`==`, hashing, matching) are unaffected.

**Aliased in-place mutation is observed.** A mutable string (from
`String.new`, `+"lit"`, interpolation, or `dup`) that is both aliased and mutated in
place shares one mutable buffer, matching CRuby's mutable String objects:
the mutation is visible through every reference and `equal?` across the
alias set is `true`. This covers the full in-place mutator surface --
`<<`, `concat`, `prepend`, `replace`, `insert`, `clear`, `slice!`, index
assignment (`s[i] = x`), `setbyte`, `bytesplice`, `append_as_bytes`, and
the transforming bang methods (`upcase!`, `gsub!`, `strip!`, `reverse!`,
...) -- across every storage shape: local aliases, array elements and hash
values (stored or read back, including mutation THROUGH a container read
like `arr[0].upcase!`), instance variables (with attr and hand-written
readers), method parameters (a callee's mutation stays visible through the
caller's aliases), returned values (including a string the callee also
retained), closure captures, and iteration variables. Frozen strings keep
raising FrozenError through every path; a hash string KEY is
snapshot-frozen on store, exactly CRuby's dup-and-freeze. Strings never
mutated in place, or mutated but never aliased, keep the plain value
representation (no cost).

#### `Range#step`, `Range#%` and `Numeric#step` return a materialized Array, not an ArithmeticSequence

CRuby's blockless `(1..10).step(2)`, `(1..10) % 2` and `1.step(5)` all return
an `Enumerator::ArithmeticSequence`: a lazy object with its own `inspect`
(`((1..10).%(2))`) and its own readers. Spinel has no ArithmeticSequence class
and materializes the stepped values at the call, as an Array. The values are
CRuby's, and so is everything computed from them: `to_a`, `each`, `map`,
`select`, `first`, `first(n)`, `size`, `sum`, `include?`, `each_slice`,
`reverse_each` and `==` all agree.

What differs is the object, not the values. `.class` answers `Array`, `p` on
the unforced sequence prints the array rather than `((1..10).%(2))`, and the
readers only an ArithmeticSequence has -- `begin`, `end`, `step`,
`exclude_end?`, `with_index` -- are not Array methods, so they raise
`NoMethodError` naming Array. A sequence that has to be described rather than
enumerated should be asked of the source range, which is unchanged.

Materializing also bounds what can be stepped at all. CRuby's sequence is lazy,
so `(1..).step(2).first(3)` and `(1..3_000_000_000).step(2).size` cost it
nothing; spinel would have to build every element, and refuses past 2**30 of
them with `RangeError: range too large to materialize`. An endless range hits
the same limit, its end being the largest representable integer.

Materializing at the call also decides *when* a bad stride is rejected. CRuby
defers the check to the point the sequence is enumerated, so `(1..10).step("x")`
returns an Enumerator, `.size` answers `nil`, and the `TypeError` arrives only
on `to_a` / `each` / `first`. Spinel raises the same `TypeError`, with CRuby's
message, at the call itself. A `String` stride on a String range differs
further: since 3.4 CRuby steps a non-numeric range by repeated `+`, so
`("a".."e").step("x")` diverges, while spinel takes every Nth element -- a
stride a String cannot name -- and raises.

#### Embedded NUL bytes: byte-exact core, C-string transforms

Strings store embedded NUL bytes, and the byte-exact core matches CRuby:
literals (`"a\0b"`), `length` / `bytesize` / `bytes`, `==` (`"a\0b" == "a"`
is false), Hash keys, slicing (`s[i]`, `s[a, n]`, ranges, `byteslice`),
`dup` / `clone`, concatenation, `0.chr`, `File.write` / `File.read`
round-trips, StringIO, pack/unpack, and Marshal.

The transforms and searches are byte-exact too: the case ops, the `strip`
family, `chomp` / `chop` / `delete_prefix` / `delete_suffix`, `index` /
`rindex` / `include?` / `start_with?` / `end_with?`, `sub` / `gsub` /
`tr` / `delete` / `squeeze` / `count`, `split` / `partition` / `lines` /
`each_line`, `reverse`, `succ`, `sum`, a regexp match position, and
padding (`ljust` / `rjust` / `center`, a `"\0"` pad included).

Interpolation and the `format` family are byte-exact too since #4632:
`"x#{s}y"`, `format` / `sprintf` / `String#%` (with a width or a
precision as well) carry the NUL and its tail, as do `IO#write`,
`#print`, `#puts` and `#pwrite`. `inspect` renders `\x00` where CRuby
prints `\u0000`. `test/embedded_nul_method_partition.rb` pins which
method is which, and `test/format_interp_binary.rb` the formatting.

#### Modules named after a builtin class

`module Encoding` at the top level is CRuby's `TypeError` (`Encoding is not a
module`) and Spinel reports the same error at compile time. A *nested*
`module Foo::Encoding` (or `class Foo; module Encoding; end; end`) is legal
CRuby -- it names a fresh constant -- and compiles: Spinel's generated C type
for a class or module is its bare tail name, which would collide with the
runtime's own `sp_Encoding`, so the nested definition and every reference to
it are qualified by their module path before the collision can happen, the
way a nested `class Array` already was. activesupport's
`ActiveSupport::JSON::Encoding` is the shape. Builtin *modules*
(`Comparable`, `Kernel`, `Math`, …) reopen normally at any nesting level.
The methods a top-level `module Kernel` reopening defines are modelled as
top-level defs (Kernel is mixed into Object, so they are bare calls from
every scope), and a `Kernel.m(...)` naming one drops its receiver, as the
builtin Kernel functions do; activesupport's `silence_warnings { ... }` is
the shape. With an explicit object receiver (`obj.twice(2)`, legal in
CRuby since Kernel's methods are public) such a method is not reached: the
call compiles and raises `NoMethodError` at run time.

#### String-named `Struct` (the `Struct::Name` form)

The legacy form `Struct.new("Foo", :a, :b)` registers the new class as the
constant `Struct::Foo`. Spinel does not support this: a class is a compile-time
entity here, and the whole point of the string name -- a class installed under
the `Struct::` namespace and reached through `Struct::Foo` -- has no analogue in
the ahead-of-time model. Spinel refuses both the string-named definition and any
`Struct::Name` reference at compile time with `Struct.new with a string name …
is not supported; use \`Name = Struct.new(...)\``. Use the modern constant-
assignment form, which is equivalent and idiomatic:

```ruby
Foo = Struct.new(:a, :b)   # not Struct.new("Foo", :a, :b)
```

#### `Rational` precision and `Complex` components

`Rational` is stored as a pair of fixed `sp_int` numerator/denominator. The
arithmetic is exact while the reduced terms fit in `sp_int`; an operation whose
result would overflow raises `RangeError` rather than promoting to a Bigint as
CRuby does:

```ruby
Rational(10**18, 1) * Rational(10**18, 1)   # RangeError (CRuby: a Bigint Rational)
```

`Complex` stores its components as `sp_float` plus a per-component class tag,
so `#real` / `#imaginary` / `#abs2` and display report `Integer` components like
CRuby for integer-valued inputs. What the representation cannot express is a
`Rational` component: operations that would produce one compute in floats
instead. This applies to exact division and to mixed `Complex`/`Rational`
arithmetic and construction, which coerce the `Rational` via `#to_f` (the
operations work; only the component class -- and therefore the printed form --
differs from CRuby):

```ruby
Complex(1, 2).real                      # => 1     (matches CRuby)
Complex(1, 2) / Complex(3, -1)          # => (0.1+0.7i)       (CRuby: ((1/10)+(7/10)*i))
Complex(1, 2) + Rational(1, 2)          # => (1.5+2i)         (CRuby: ((3/2)+2i))
Rational(1, 2) + Complex(1, 2)          # => (1.5+2i)         (CRuby: ((3/2)+2i))
Complex(Rational(1, 2), Rational(1, 3)) # => (0.5+0.3333333333333333i)
                                        #                     (CRuby: ((1/2)+(1/3)*i))
Rational(3, 4).i                        # => (0+0.75i)        (CRuby: (0+(3/4)*i))
```

`Rational` and `Complex` values box into heterogeneous (poly) arrays and hashes
normally.

#### Negative Float `**` fractional exponent

CRuby promotes `(-2.0) ** 0.5` to a `Complex`. Spinel's Float stays a C
double, so that case raises `Math::DomainError` loudly (the class
`Math.sqrt(-1)` uses) rather than returning C's silent `NaN` or widening
every float power to a boxed union. Compute via `Complex(x) ** y` where the
complex result is really wanted.

#### `const_get` with a runtime name is a static dispatch

Constants are resolved at compile time, and every constant the program
defines is known then, so `const_get` with a name known only at run time
lowers to a dispatch over those names, the way a runtime `send` lowers over
the program's method names: the arm whose name matches answers the constant,
a class or a value, and a name matching none raises the `NameError` CRuby
raises (`uninitialized constant Carts::Nope`, `wrong constant name lower`).

```ruby
module Carts
  TYPES = { 0 => "A", 1 => "B" }
  def self.build(t, x) = const_get(TYPES.fetch(t)).new(x)   # works
  def self.constantize(name) = Object.const_get(name)         # activesupport's shape
end
```

The result is poly (any constant), so what follows is the boxed-value
surface: `.new`, `.name`, `::CONST` on a class value all work. The
candidates are the program's constants by their own name in the flat
namespace the literal form resolves in, so a `"Outer::Inner"` path string is
not matched and raises. The call needs a class or module receiver, explicit
or the implicit self of a class method or class body; in an instance method
an instance has no `const_get`, and the call is refused where it is written.

#### `defined?(@ivar)` is answered at compile time

CRuby answers `defined?(@ivar)` from the object's runtime state: `nil` until
the instance variable is first assigned, `"instance-variable"` after -- which
is what makes it usable as a memoization guard for falsy values
(`return @x if defined?(@x)`).

Spinel's instance variables are C struct fields. Every ivar the program
mentions exists in the object's layout from allocation, pre-filled with its
type's nil representation; there is no per-object "has been assigned" record
to consult. `defined?(@ivar)` therefore folds at compile time: it is truthy
iff the program contains an assignment to that ivar anywhere, regardless of
whether *this* object has been assigned yet at run time. The falsy-value
memoization pattern silently reads the unassigned slot on the first call:

```ruby
def foo
  return @foo if defined?(@foo)   # compile-time truthy: an @foo= exists below
  @foo = compute                  # never reached -- foo returns nil forever
end
```

Tracking runtime assignment would need a shadow presence bit per ivar written
on every assignment -- cost on every object and every ivar write to serve a
rare pattern. Use a nil check (`@foo = compute if @foo.nil?`, i.e. `||=`) when
`compute` never yields nil/false, or an explicit sentinel/flag ivar when it
can:

```ruby
def foo
  return @foo if @foo_set
  @foo_set = true
  @foo = compute
end
```

#### `Hash#compare_by_identity`

`compare_by_identity` is rejected at compile time (never silently ignored).
Spinel's hashes are typed storage variants keyed by VALUE -- string keys hash
and compare by content, and string literals are shared through a frozen pool.
Identity-keyed comparison cannot be honored on that representation: two
equal-content String keys may be the *same* object in Spinel where CRuby sees
two distinct ones, so even a dedicated identity mode would diverge from CRuby
on the exact programs that need it. Restructure identity-keyed lookups to use
an explicit unique key (an Integer id, a Symbol) instead.

#### `String#equal?` and literal identity

`equal?` on strings is pointer identity. Each literal OCCURRENCE compiles
to its own frozen static object (see the identity note under the
frozen-string-literal section): `"x".equal?("x")` is `false`, and
re-evaluating one occurrence (a literal in a loop) yields the same object.
Both facets are implementation-defined under `frozen_string_literal`
semantics and programs should not depend on them. Everything else about
identity is truthful: `s.freeze.equal?(s)` is `true` (freeze marks in
place), aliasing compares equal, `-lit` dedups interned content to one
object, and distinct-valued strings compare `false`.

#### `defined?`

`defined?` is resolved **statically at compile time** from the operand's
syntactic form and whole-program symbol presence, not from the runtime state of
the actual receiver or binding. It returns a fixed label string (or `nil`),
which matches CRuby for the common forms but differs in several cases. A full
runtime-accurate `defined?` would require carrying per-object/per-binding
definedness into the generated code; that cost is deliberately not paid.

Forms that match CRuby: a local variable (`"local-variable"`), a set/unset
instance variable, a set/unset global variable, a user or built-in constant
name (`"constant"`), a no-receiver call to a **user-defined** method
(`"method"`), `self`, `nil`/`true`/`false`, and the int/float/string/symbol/array
literals that report `"expression"`.

Where Spinel returns `nil` but CRuby returns a label:

| Operand | CRuby | Spinel |
| --- | --- | --- |
| `Foo::Bar` (constant path) | `"constant"` | `nil` |
| `puts` (built-in / Kernel method) | `"method"` | `nil` |
| `obj.meth` (call with a receiver) | `"method"` | `nil` |
| `1 + 1` (operator = method call) | `"method"` | `nil` |
| `{a: 1}`, `1..3` (hash/range and other general expressions) | `"expression"` | `nil` |
| `x = 1` (assignment) | `"assignment"` | `nil` |
| `yield`, `super` | `"yield"` / `"super"` | `nil` |
| Multi-encoding strings | Strings are UTF-8 or ASCII-8BIT, and one rule says which: a string is ASCII-8BIT when the program ASKED FOR BYTES (`pack`, `String#b`, `Marshal.dump`, `Random#bytes`, `binread`, `unpack`'s byte directives, `force_encoding` naming it). Everything else is UTF-8, including what CRuby calls US-ASCII (see below). A string carries one bit, not an encoding object; `#length`, `#[]`, `#inspect`, `#encoding` and the comparisons read it. A compiled binary's string paths (indexing, regexp, hashing) assume the two share a byte representation, and that assumption is load-bearing for their performance | write UTF-8; transcode at the boundary before the data enters the program |

Two forms report a label where CRuby would report `nil`, because the check is
syntactic rather than runtime:

- An instance variable reports `"instance-variable"` when **any** code in the
  program assigns that ivar name -- not whether it is set on the specific
  receiver at that point.
- A class variable read always reports `"class variable"`, with no
  definedness check, so `defined?(@@undefined)` is `"class variable"` in Spinel
  versus `nil` in CRuby.

#### `String#grapheme_clusters`

Correct Unicode extended-grapheme segmentation (`"á".grapheme_clusters # => ["á"]`)
requires shipping and maintaining the Unicode grapheme-break property tables,
which Spinel deliberately does not carry. `String#grapheme_clusters` and
`String#each_grapheme_cluster` are therefore not supported. For codepoint- or
byte-level iteration, use the supported `String#chars`, `#each_char`,
`#codepoints`, or `#bytes`.

#### `String#unicode_normalize`

Unicode normalization (`"é".unicode_normalize(:nfc) # => "é"`) requires
shipping and maintaining the Unicode decomposition/composition tables, which
Spinel deliberately does not carry -- the same limit as
`String#grapheme_clusters` above. `String#unicode_normalize`,
`#unicode_normalize!`, and `#unicode_normalized?` are therefore not supported,
and a call to them is rejected at compile time.

#### `Time` sub-nanosecond precision

Spinel's `Time` stores an `int64` second count and an `int32` nanosecond
fraction (nanosecond resolution). CRuby keeps the exact rational a `Float` or
`Rational` argument produces, so it carries bits below one nanosecond.
`#nsec` / `#usec` agree with CRuby (both truncate to the nanosecond), but two
things differ: `Time.at(f).to_f` does not always round-trip a `Float` (CRuby
rounds the exact rational to the nearest double; Spinel reconstructs from the
nanosecond value, so `Time.at(12345.678).to_f` is `12345.677999999`), and
`#subsec` returns a nanosecond-resolution `Rational` (`Time.at(2.2).subsec` is
`(1/5)`) rather than the exact binary fraction of the source `Float`.

#### Aliasing the regexp match globals

CRuby's `English` library aliases the punctuation match globals to readable
names (`alias $MATCH $&`, etc.). In Spinel the match globals (`$&`, `` $` ``,
`$'`, `$+`, `$~`) are not ordinary global-variable storage: a direct read lowers
to a special regexp runtime accessor. Supporting `alias $name $&` would require a
separate special-global alias mechanism plus broader `MatchData` compatibility,
outside the intended AOT subset. Aliasing one of these globals is rejected at
compile time rather than falling through to an undefined generated symbol:

```
$ spinel uses_english.rb
Error: global aliasing of regexp special globals is not supported (alias $MATCH $&)
```

Direct reads of the match globals work as usual; only aliasing them is
unsupported, so `require "English"` does not compile.

#### Flip-flop operator

CRuby supports the flip-flop operator (a `Range` used as a condition, toggled
between its two endpoints): `puts i if (i == 3)..(i == 5)`. This is a rarely used
feature with surprising hidden per-site state, and Spinel does not support it; a
program using it fails to compile rather than running with wrong behavior. Use an
explicit boolean state variable instead.

#### No `US-ASCII`

CRuby has three encodings where spinel has two. A string CRuby generated from
nothing -- `1.to_s`, `nil.to_s`, `:sym.to_s`, `1.chr`, `[1, 2].inspect`,
`Time#to_s` -- is US-ASCII; one derived from source text inherits the source's
encoding. Spinel calls both UTF-8.

The reason this costs nothing is that US-ASCII carries exactly one fact, "these
bytes are 7-bit", and nothing else. It is not needed for compatibility:
`rb_enc_compatible` keys on the CONTENT being 7-bit, not on the encoding's
identity, so an ASCII-only UTF-8 string concatenates with a Shift_JIS one
exactly as a US-ASCII string does. Across the operations that can tell two
same-byte strings apart -- `==`, `eql?`, `<=>`, `hash`, Hash keys, `length`,
`[]`, `chars`, `bytes`, `upcase`, `include?`, `index`, `sub`, `split`, regexp
matching, `to_sym`, `ascii_only?`, `valid_encoding?`, `encode`, `unpack`,
`force_encoding`, concatenation in both directions -- US-ASCII and ASCII-only
UTF-8 agree on every one. What differs:

```ruby
1.to_s.encoding      # CRuby: US-ASCII      Spinel: UTF-8
(1.to_s + "x").encoding
                     # CRuby: US-ASCII      Spinel: UTF-8
1.chr.inspect        # CRuby: "\x01"        Spinel: "\u0001"
```

The encoding's NAME, and `inspect`'s escape form for a non-printable byte.
CRuby needs the name because encodings are first-class objects and every string
must report one; a program cannot name an encoding in spinel, so there is
nothing for the third name to distinguish. The 7-bit fact itself is not lost --
spinel keeps it as a bit in the string header, where it makes indexing O(1)
rather than naming anything.

#### No `Encoding::CompatibilityError`

CRuby raises `Encoding::CompatibilityError` when a two-string operation
(`include?`, `index`, `+`, `sub`, `start_with?`, ...) is handed operands whose
encodings are incompatible and whose bytes are not all ASCII:

```ruby
"café".include?("é".b)   # CRuby: Encoding::CompatibilityError
                         # Spinel: true
```

The error guards against a byte match that is not a character match, which is
a real hazard when the two operands are, say, Shift_JIS and UTF-8: the same
bytes mean different characters. Spinel has two encodings, UTF-8 and
ASCII-8BIT, and they share one byte representation -- ASCII-8BIT is bytes with
no character interpretation at all, so there is no second interpretation for
the first one to disagree with. The failure the error exists to prevent cannot
happen here, so Spinel answers the byte question instead of refusing it.

Where CRuby produces a *value* rather than an error, Spinel matches it.
`String#==`, `#eql?`, `#<=>` and `#hash` follow CRuby's `rb_str_comparable`:
equal bytes are equal strings only when the encodings are comparable -- the
same encoding, or both operands ASCII only. That matters beyond the comparison
itself, because it decides whether a Hash keeps a binary blob and a text
string as one key or two.

The visible consequence of drawing the line there is an asymmetry:

```ruby
"café".include?("café".b)   # true  -- a byte search
"café" == "café".b          # false -- CRuby's answer, and the Hash-key rule
```

CRuby has the same pair; it just answers the first with an exception rather
than with `true`.

---

## Now supported (older write-ups are stale here)

These were limits in an earlier (Ruby self-hosted) version of the compiler and
now work on current master:

| Feature | Status |
|---|---|
| Mutable strings and aliased in-place mutation (`s = +"x"; s << "y"`; literals are frozen by default -- see the String section) | works |
| Hash missing key → `nil` (string- and int-keyed, including `Hash.new(default)`) | works |
| `define_method(:name) { ... }` with a literal name | works |
| Block-param arity (un-yielded params are `nil`, not a sentinel) | works |
| Closures flowing through containers (`{op: ->(a,b){a+b}}[:op].call(2,3)`) | works |
| `String#oct` (`0x`/`0b`/`0o` prefixes) and `Array#first` on empty → `nil` | works |
| `send(:literal)` / `__send__("literal")` / `public_send(:literal)` on **implicit self** | works (resolved on the AST, so a `send(:` inside a string literal is left untouched) |
| `alias` / `alias_method` inside a reopened `String` / `Integer` / `Float` / `Symbol` (`class String; alias starts_with? start_with?`), naming a builtin method or one the reopen defined | works on a concretely typed or implicit-self receiver; a poly (run-time-typed) receiver does not see the alias yet |
| A program's own instance methods on `Range`, `Time`, `File` and `Class` (`class Range; def blank? = false`, `def span = last - first`), and class methods on `File` | works on a concretely typed receiver, with `self` the builtin value and a receiverless builtin call (`last`) resolving on it; a poly (run-time-typed) receiver reaches a `Range` / `Time` method too. Used to be a C typedef collision (`sp_Range`) before any call |
| activesupport's `blank.rb` shape: reopens of `Object`, `NilClass`, `TrueClass` / `FalseClass`, `Array`, `Hash`, `Symbol`, `String`, `Numeric` and `Time` each defining `blank?` / `present?`, `present?` calling the reopen's own `blank?` bare, `alias_method :blank?, :empty?` on `Array` / `Hash` / `Symbol`, and `Object#blank?` asking `respond_to?(:empty?)` of a self that may be anything | works: every receiver kind, concretely typed or poly, reaches its own class's definition (nil is blank, `[]` is blank, a user object answers through `Object`'s), and `Integer` / `Float` fall to `Numeric`'s. Inside an `Object` / `Numeric` reopen a bare call goes through `self`, so `present?` inside `Object#presence` reaches `String#present?` for a String as Ruby's lookup does. An `Array` / `Hash` reopen method is reached on a concretely typed receiver and through a poly receiver holding a container of any element kind |
| Hash variant inference (a wrong initial guess widens to poly transparently) | correct (a perf cost, not a correctness limit) |

There is no Ruby self-host "bootstrap fixpoint" constraint: the C compiler is
the master implementation.

---

## Why this still works

Most real programs use the dynamic features above sparingly, in setup code, or
not at all. Spinel targets the large static core of Ruby -- classes, methods,
blocks, the collection protocols, exceptions, mixins -- and compiles it to fast
native code. When a program does need a feature in the *fundamental* table, that
program is not a fit for AOT; for everything else, the limits are either by
design or on the relaxable list.
