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

A construct rejected at compile time is a *refusal*: a
`spinel: FILE:LINE: ...` line on stderr naming the construct. By default, one
run reports every refusal it encounters in the program
(each abandons the method it is in and the compile goes on to the next), then
fails once with the count and writes no binary. `--emit-types` carries the same
refusals in its `diagnostics` array with `"severity":"error"`.

Some unsupported constructs compile and fail only when reached at runtime.
For example, a computed `instance_variable_set` on a user-class instance can
raise `NoMethodError`; computed getters on such receivers are refused at
compile time. `define_method` with a non-literal name, in a class body or a
method body, and `Class.new` with a non-constant superclass are refused at
compile time. `Class.new` and `Module.new` blocks that capture outer locals
are refused at compile time unless `--defer-refusals` is supplied.
A successful compile does not establish CRuby-compatible behavior.
Separately, `--defer-refusals` explicitly turns eligible compile-time
refusals into runtime `NotImplementedError`s.

---

## Fundamental limits (inherent to AOT)

These need a runtime parser, a runtime metaobject protocol, an allocation
registry, or stack reification -- none of which exist in a flat compiled binary.

| Feature | Behaviour | Why it's fundamental |
|---|---|---|
| `eval` / `instance_eval("str")` / `class_eval("str")` | unsupported | needs a runtime parser + type system. (Block forms -- `instance_eval { }` -- DO work; the block is compiled.) Code in a branch a `RUBY_ENGINE == "..."` check rules out (`if RUBY_ENGINE == "spinel" ... else eval(...) end`) is dropped before analysis, so a library can keep an eval backend for other engines. |
| `method_missing` | not dispatched (defining it warns at compile time) | every call site is a direct C call; an undefined-method call can't fall back to a per-receiver hook. The method is still callable explicitly. |
| `define_method` with a runtime-computed name/body | calls with a non-literal name, in a class body or in a method body (instance or class method), are refused in reachable code, even when the defined method is never called; an unreachable method and pruned code are not refused. A program that defines or aliases its own `define_method` keeps those calls as ordinary calls; that check is program-wide, so a `def define_method` in an unrelated class turns the refusal off everywhere, and a computed-name class-building call then raises `NoMethodError` at run time as it always did. `send(:define_method, name)` is not refused either. Literal Symbol/String names and existing literal-list unrolling work. A constant holding a Symbol or String name is not supported yet | the compiled method table cannot acquire a runtime-built method |
| `ObjectSpace` (`each_object`, `count_objects`) | unsupported | no class-keyed allocation registry; the GC tracks bytes, not a live-object index. `define_finalizer` / `undefine_finalizer` *are* supported (the collector watches the object; the callable runs at the first safe point after the collection that frees it -- a method entry, a loop back-edge, `GC.start` -- or at exit) |
| `TracePoint` / `set_trace_func` | unsupported | require an interpreter loop to hook |
| `binding` as an object | unsupported | reifying the local scope needs a runtime name->slot table; locals are C stack slots. `binding.local_variable_get(:x)` with a **literal** name *is* supported -- it resolves to the known slot at compile time |
| Refinements (`refine` / `using`) | no-op / unresolved | scope-keyed dispatch is incompatible with direct C calls |
| A seeded `Random.new(n)` or `srand(n)` | draws a different sequence from CRuby's | the generator is PCG, not Mersenne Twister, so `Random.new(1).rand(6)` is `0` where CRuby answers `5`; a program that prints seeded draws differs from CRuby's, and one that only needs them reproducible does not |
| `callcc` / `Continuation` | unsupported | multi-shot full-stack capture has no flat-C analogue |
| `Class.new(parent)` with or without a block | static forms work; non-constant superclass expressions are refused at compile time | the class graph is baked at compile time; see [Class.new cases](#classnew-cases) below |
| An instance variable of a String (`@x = v` in a method added to String, `s.instance_variable_set(:@x, v)`) | refused at compile time, until Strings are shared rather than copied (#6765); one reached through an untyped value raises NotImplementedError when it runs, as does one on a Time | a String is copied between its representations and across calls, so it has no one identity yet; under #6765's share-by-default model it keeps one and takes the same map as an Array. A Time is copied by value. An Array, a Hash, a Random, a Proc, an exception and a class value keep their instance variables, in a table keyed by the object (as CRuby's); a class value's own class-level slots stay where its class methods read them, and its `instance_variables` lists those only its class methods wrote after the reflective sets. An Integer, a Float, a Symbol, nil, true, false and a Range read nil and raise FrozenError on a write, as in CRuby. An ivar of a builtin value as a multiple-assignment target (`@a, @b = x, y` in an Array method) is refused: assign each on its own |
| A subclass of a builtin value class: `class Name < String`, and likewise Range, Proc, Method, UnboundMethod, Integer, Float, Symbol, Rational, Complex, NilClass, TrueClass, FalseClass, Regexp, MatchData, Time, Random, Enumerator, IO, File, Dir, Thread, Fiber, Mutex, Queue, SizedQueue, ConditionVariable, OpenStruct, or a class a package binds to C (StringIO); also `Thread::Queue` | refused at compile time, naming the class | the subclass would be built as a plain object: none of the parent's methods reach it, its constructor takes none of the parent's arguments, and `p`, `to_s`, `==` and `respond_to?` answer as for an Object. Supporting it needs an instance that IS a String (Range, ...) with the subclass's methods dispatched on it, as an Array or a Hash subclass's is (below); until then, keep the value in an instance variable of a class of your own. A subclass of Object, BasicObject, an exception, Struct / Data, Numeric, or a package class written in Ruby (Set, Date, BigDecimal) works, as does a class of the program's own that shares a builtin's name under a namespace (`Jobs::Queue`) |
| A subclass of Array (`class Page < Array`, `::Array`, `Class.new(Array) do ... end`) | supported (#7449), except the shapes in the next column, which are refused at compile time | its instance IS its Array: the struct starts with the Array, so Array's methods run on it, a boxed one is an Array to the runtime, and its class is read back off its own GC scan function. Refused: `Class.new(Array)` without a block (no class of the program's own stands for it; write `class Name < Array`), and a bare `super` into Array from a method with keyword, post-rest or destructured parameters (pass the arguments explicitly). `Marshal.dump` writes an instance as CRuby does (`C` with the class, the elements, and its ivars under `I`), and `Marshal.load` reads that back as an instance of the class, whether Spinel or CRuby wrote it |
| A subclass of Hash (`class Registry < Hash`, `Class.new(Hash) do ... end`) | supported, except the shapes in the next column, which are refused at compile time | its instance IS its Hash, as an Array subclass's is its Array: Hash's methods run on it (`merge`, `compact`, `dup` and `clone` answer an instance of the class, `select`, `slice`, `to_h` a plain Hash, as in CRuby), `super` in `initialize` takes Hash.new's default and default block, and a reopen of `Hash` beside it (activesupport's core extensions) gives it the reopen's methods. An `alias_method` of one of Hash's methods ahead of the class's own definition (`alias_method :regular_writer, :[]=`) keeps naming Hash's, in an Array subclass too. Refused: `Class.new(Hash)` without a block, and a bare `super` into Hash from a method with keyword, post-rest or destructured parameters |
| Singleton methods (`def obj.m`, `class << obj; def m; end; end`, `obj.define_singleton_method(:m) { }`, `obj.extend(Mod)`) on a receiver whose creation site is **not** visible | unsupported | these DO work when the receiver is a constant or a local whose only write is `<UserClass>.new(...)`: the object gets a synthesized anonymous subclass carrying the methods, which is the AOT form of CRuby's hidden singleton class. What is left out is a receiver spinel cannot trace to one `.new` (a factory return, a loop, a conditional), and one whose class has no subclassable layout: `Object.new` / `BasicObject`, a builtin (String, Array), a Struct or Data, an exception. Those are refused at compile time, naming the Ruby line, when the body needs a `self` (its own `@ivar`, or `self`); a body that needs neither compiles as an ordinary function and is simply never reached as a method |
| Singleton definitions (`def Queue.helper`, `class << Queue`) reopening a builtin whose class body is unsupported | refused at compile time | the same reopening limits apply to reachable singleton methods, including Queue, SizedQueue, Mutex, ConditionVariable and Encoding. An unreachable singleton method is ignored; class-body reopenings remain refused. |
| `alias` / `alias_method` capturing a builtin singleton method before redefining it | refused at compile time | the alias must keep the original builtin implementation. Redirecting it to the later user override could recurse. An alias of a user singleton method already defined remains supported. Calling a captured builtin singleton alias through a constant is also refused. |
| `super` from a singleton override to an inherited builtin singleton method, such as `File.read` or `File.open` reaching `IO`, or `Kernel.puts` reaching the Kernel instance method through Object | refused at compile time | this dispatch has no compiled user method to call. A `super` that resolves to a user singleton method remains supported; a method with no superclass implementation still raises `NoMethodError`. |
| `Object#singleton_class` as an OBJECT (and `Class#attached_object`) | unsupported | the singleton class above is synthesized, not reified: there is no runtime class object to hand back. `class << obj` as a *definition* form works -- see the row above. `singleton_class.prepend(Mod)` / `singleton_class.include(Mod)` as a statement of a class or module body (activesupport's const_missing hook on Enumerable) is read as `extend Mod`, the precedence between Mod and the class's own singleton methods aside |
| Runtime structural mutation of a class through an explicit receiver (`Klass.include(M)`, `Klass.attr_accessor(...)`, `Klass.define_method(...)` outside the class body) | unsupported | the class graph, ancestor chain, and method/ivar layout are baked at compile time; the same declarations *inside* a `class` body work |
| A call through an `@ivar` before the method that assigns it has run (`@store[k] = v` ahead of a `reset` that sets `@store = {}`) | a release build dereferences the unset slot and crashes (SIGSEGV); a `-g` / `--debug` build raises CRuby's `NoMethodError` (`undefined method '[]=' for nil`) | the test would stand in front of every call through such an ivar, and state a `reset` or `setup` method assigns is often the hottest there is (optcarrot lost 10-20% to it), so only the debug build carries it. An ivar the program fills only by memoization (`@c ||= {}`) is guarded in both builds |
| An ivar write in an Object or Kernel method with boxed `self` | uses the existing reflective setter; refused if a possible receiver has no writable slot, or the default build encounters a shared String handle slot; `--share-strings` carries the handle through the setter | ordinary user-class slots are registered by the existing boxed-receiver analysis, including a class that starts with no ivars; its indexed call sites bound the receiver layouts, while calls it cannot follow remain conservative; an unused setter adds no instance slots; no write is silently dropped |
| `defined?(@x)` in an Object or Kernel method whose receiver is boxed | refused when the existing layout facts cannot track whether a possible receiver's slot has been assigned | an initialized slot, a slot whose value distinguishes unset, or a slot with an explicit presence bit is supported; a slot introduced by a root setter keeps its presence bit even if an unrelated class writes an ivar of the same name; mixed ordinary/reflective nil writes to related layouts can leave presence untracked |
| General reflection (`methods`, `instance_variables`) and `instance_variable_get`/`set` with a **non-literal** name | non-literal `instance_variable_get` calls whose receiver may be a user-class instance are refused in reachable code, including input-dependent branches; computed `instance_variable_set` on user instances remains unsupported and can raise `NoMethodError` at runtime | user-class ivars are C struct offsets; literal access resolves to the known offset. Builtin reflective paths and ordinary ivar access in Object methods keep working. Computed setters are not refused: `test/reflect_ivar_dynamic_name.rb` contains a reachable conditional setter whose untaken branch already works. `instance_variables` can list these known fields. A **literal** `instance_variable_get(:@x)` / `instance_variable_set(:@x, v)` *is* supported, like `send(:literal)` below. |
| User-defined `#hash` / `#eql?` for hash *keys* | not dispatched (identity probe) | the hash machinery can't call back into a user method per key |
| A method that **uses its block** (`yield` or `block.call`) **and recurses into itself** (`def rec(n, &b); ...; rec(n-1, &b); yield n; end`) | compile error (loud, was a hang / undefined-symbol) | a block-using method is inlined at each call site (there is no standalone function that takes the block), so a self-call inlines its own body unboundedly -- the runtime base case is invisible at compile time. Recursion *through a yielded block* (`with_state { with_state { } }`, finite source nesting) does work |
| `Monitor#class` | reports `Thread::Mutex` | a Monitor IS a mutex here, with reentrancy switched on per object, and the class name for a `TY_MUTEX` value is decided at compile time from the type rather than read off the object. `#synchronize` (including reentrant use), `#try_enter` and mutual exclusion across threads all behave as CRuby's do; only the name differs. `Monitor#new_cond` / the `MonitorMixin` module are not modelled. |
| `require` of stdlib `.rb` that leans on metaprogramming / C extensions (e.g. `json/pure`) | unsupported | such stdlib code runs off the AOT path. A `require` is resolved at parse time by splicing a bundled `lib/X.rb`; the libraries that ship this way -- `set`, `forwardable`, `optparse`, `erb`, `csv`, `pathname`, `stringio`, `strscan` -- do work. |
| `Time.parse` / `Time.strptime` / `Time.iso8601` / `Time.xmlschema` / `Time.httpdate` / `Time.rfc2822` / `Time.rfc822` (the `require "time"` string-parsing additions) | refused at compile time, naming the limit | the built-in `Time` class (`Time.now` / `at` / `local` / `utc`, plus `strftime` / `zone`) works *without* any `require`, and `require "time"`'s other additions (the instance methods `iso8601`, `httpdate`, `rfc2822`) work too; only parsing a String into a Time is missing. Store times as epoch seconds and read them with `Time.at` instead. A program that reopens `Time` with its own class method of one of these names keeps calling that method, not this limit. |

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
`OpenSSL::Digest` (`SHA256` / `SHA1` / `MD5`, the class-method forms and the
object: `OpenSSL::Digest.new("SHA256")`, `update` / `<<`, `digest`,
`hexdigest`, `digest_length`), `OpenSSL::HMAC`, `OpenSSL::KDF.hkdf` /
`pbkdf2_hmac` and `OpenSSL::PKCS5.pbkdf2_hmac` are there, over the runtime's
own crypto rather than libssl, and no HMAC-MD5; the digest object buffers its
input and hashes it whole. `Cipher` (aes-gcm) and `PKey::EC` are subsets,
each described in its file under `packages/openssl/openssl/`. Most of `X509`
is not there: a call to a method the package does not define compiles into
CRuby's NoMethodError, raised when it is reached. Spinel
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

### `Class.new` cases

The assignment target, superclass expression and presence of a block all
matter. Assume an ordinary user-defined `class Base; end` and `base = Base`.
Each row below is a separate program; the blocks shown capture no outer locals.

| Assignment | Compile | Run / subsequent use |
|---|---|---|
| `Foo = Class.new(Base) {}` | succeeds | works |
| `foo = Class.new(Base) {}` | succeeds | `name` is nil until a constant assignment executes; `to_s`/`inspect` use the anonymous class address form |
| `Foo = Class.new(Base)` | succeeds | works |
| `foo = Class.new(Base)` | succeeds | same anonymous naming as the block form |
| `Foo = Class.new(Mod)` or `foo = Class.new(Mod)` with `Mod` a module | succeeds | raises `TypeError` where the call runs, as CRuby does |
| `Foo = Class.new(base) {}` | refused | no binary |
| `foo = Class.new(base) {}` | refused | no binary |
| `Foo = Class.new(base)` | refused | no binary |
| `foo = Class.new(base)` | refused | no binary |

An instance of an unnamed class inspects as `#<#<Class:0x...>:0x... @a=1>`,
and as `#<Foo:0x... @a=1>` once a constant assignment names the class.

At top level or in a class body outside a method, loop or block, omitting
the block with a constant superclass is equivalent to an empty block.
No-block calls in a method, loop or block retain their existing handling;
this lowering does not give them fresh class identity on each evaluation. A local holding a class is still not a supported superclass expression.
For example, the first assignment works, but the second is unsupported:

```ruby
class Base; end
foo = Class.new(Base)             # works
bar = Class.new(foo)
```

Use constants for the superclass expressions to build the chain statically:

```ruby
class Base; end
Foo = Class.new(Base) {}
Bar = Class.new(Foo) {}
p Bar.superclass == Foo           # true
```

Spinel lowers the supported forms to static class definitions when the superclass
is omitted or named by a constant and the block captures no outer locals. A
constant assignment names the class; a local assignment or other expression
uses a synthesized class. A method can return such a class too:

```ruby
class Base; end
k = Class.new(Base) { def hi = 1 }
p k.new.hi                                  # 1

def make = Class.new(Base) { def hi = 1 }
p make.new.hi                               # 1
p make == make                              # true (CRuby: false)
```

Each evaluation of the same supported `Class.new` expression yields the same
compiled class; CRuby creates a new class on each evaluation. Factories with
a block remain accepted, including programs that do not observe class identity.
Calls without a block in a method, loop or block are not lowered, so they gain
no support for repeated construction.

A `Class.new` or `Module.new` block that captures outer locals is refused at
compile time for constant assignments, local assignments and reachable method
returns. An unreachable method remains pruned, including any captured
class-building block in its body. A Proc or lambda that nothing reads, whether
held by a local, a constant, an instance variable or a global, does not consume
the capture refusal either. Reachability is approximate in the other
direction: a Proc passed to a method that never calls it, and a method called
only from an uncalled lambda, count as reachable, so a capture inside them is
still refused. With `--defer-refusals`,
it retains the runtime `NotImplementedError` instead. Locals defined inside
the class-building block are not outer captures.

A non-constant superclass expression is refused in reachable code, with or
without a block and regardless of the assignment target. Pruned scopes are
not refused. Even `base = Base`
aliases are not supported yet; use `Class.new(Base)` instead. A constant alias
of a class (`S = Base; Class.new(S)`) is accepted but loses Base's methods, as
it did before.

Constant builtin parents use the same lowering where supported; for example,
`Err = Class.new(StandardError)` works without a block. Array and Hash retain
the no-block refusals described above.

---

## Partial / relaxable limits

Limited today, but additively fixable; listed roughly easiest-first.

| Feature | Today | Path to relax |
|---|---|---|
| `Exception#backtrace` / `Kernel#caller` | return `[]` in a release build (class + message work). A `--debug` build, or `-g` with `-O0` / `-O1`, names each frame `file:line:in 'Class#method'` with the file the method was written in and the line of the call. The line comes from the build's own debug info, through `addr2line` on Linux (which must be installed) and `atos` on macOS (reading the `.dSYM` the build leaves beside the program); without them a frame has no line. `-g` at the default `-O2` drops the frames the C compiler inlined (a method called from one place usually is), so use `--debug` for a backtrace | a release build would need a pc→line table in every binary (#7658) |
| SIGINT / SIGTERM with no `trap` | a program that starts no thread raises `Interrupt` (`#signo` 2) / `SignalException` (`#signo` 15) in the main thread, as CRuby does, so `rescue` and `ensure` run; unrescued, it flushes its output and ends by the signal (a parent reads 130). A program that starts a thread keeps the system default: the process ends at once, with no `rescue` or `ensure` | deliver the raise to the main Ruby thread through the scheduler, whichever OS thread the signal reaches |
| `class Thread` / `class Fiber` reopenings, `Thread.attr_accessor :x` / `Fiber.attr_accessor :x` (activesupport's IsolatedExecutionState) | supported | a reopening's instance methods take the runtime handle as self, and `Thread.current` / `Fiber.current` reach them (also through a class value or a class held in a poly slot). A thread's attribute lives in its thread-local table under a private key; a fiber's in a table of the fiber's own that a new fiber does not inherit (an attribute on a fresh fiber is nil). `thread_variable_get` / `_set` / `?` share the store `Thread#[]` / `[]=` / `key?` keep: one table per thread for both, where CRuby's `[]` is fiber-local |
| `Thread` real parallelism | implemented as a true M:N runtime (no GVL): N OS workers (`min(online cores, SPINEL_WORKERS)`) run green threads in parallel over a stop-the-world GC, with real `Mutex`/`Queue`/`SizedQueue`/`ConditionVariable`. A monitor thread timeslices CPU-bound threads (~10ms quantum) so a thread looping without yielding cannot starve its siblings (it signals the worker with `SIGURG`, overridable via `SPINEL_PREEMPT_SIGNAL`). The single-threaded archive is unchanged (a non-`Thread` program is byte-identical) | the N workers run per-worker run queues with work stealing, and `Kernel#sleep` and blocking I/O are scheduler-aware (a sleeping / I/O-blocked thread frees its OS worker). preemption is taken at safepoint polls (loop back-edges), so a thread spending a long time inside a single runtime call with no poll yields only when that call returns; concurrent allocation is thread-safe (heap-lock-protected allocators, atomic heap byte counters, per-worker object pools) but every allocation still crosses one global heap lock; remaining work: fully async (signal-interrupted) preemption of such regions, and per-worker allocation buffers (TLAB) to make allocation-heavy parallel code scale. See [docs/thread.md](thread.md) |
| `Marshal` of user objects with container-typed ivars | primitives + Array + Hash + Bignum + Complex + Rational + plain user objects work, including cyclic and shared references (`Marshal.dump`/`load`, CRuby 4.8 wire format, byte-compatible for the supported subset); an object whose ivar is a *statically typed* Array/Hash (not a poly ivar) is not yet dumpable | a user object dumps/loads through a compile-time-generated per-class dispatcher. Supported ivar types: scalars (Integer/Float/String/true/false/Symbol/Bignum), `poly` (mixed) ivars, and nested user objects. A typed-container ivar would mismatch the loader's always-poly containers, so such a class raises `TypeError` on dump; value-type and Exception-subclass objects are also out of scope. Complex's components are float-only, so they round-trip as Floats |
| Mixin lifecycle hooks (`def self.included(base)` / `def self.extended(base)` with one required parameter) | fired for `include M` in a class body and `extend M` in a class or module body, with a literal module name | the compiler splices the hook body after the declaration and substitutes the receiving class/module for `base`; this is static expansion, not general runtime hook dispatch |
| Inheritance lifecycle hook (`def self.inherited(sub)`, `def inherited` in `class << self`, or `def inherited` in a module the class extends) | fired once per subclass, before its body runs, for `class A < Base` with a constant superclass (one built by `Struct.new(...) do ... end` too), through the nearest ancestor that defines it (a private hook too) | the compiler places `Base.__send__(:inherited, self)` first in the subclass's first body; this is static expansion, not runtime hook dispatch. `X = Class.new(Base) do ... end` fires it with the class already named `X` (CRuby names it after the hook returns); `Class.new(Base)` with no block, a superclass held in a variable, and a module extended through anything but a constant, do not fire it |
| Methods added to `Class` (`class Class; include M; end`, `Class.class_eval { include M }`, a `def` in either) | a class method of every class: `Klass.m`, `String.m`, and a bare `m` in any class body. Refused at compile time, naming the line: `Class.class_eval` anywhere but a top-level statement, and `class Class < ...`. On a builtin class a class-level `@ivar` of such a method is one slot shared by every builtin class | a nested or conditional addition would need the methods to exist only once it has run; a builtin class has no class-ivar storage of its own |
| External `Enumerator` -- `.each` with no block is only an Enumerator on `Array` / `Range`, not on an arbitrary user method | mostly supported | `Array#each` / `Range#each` with no block return a working external Enumerator (`#next` / `#peek` / `#rewind` / `#size`, `loop` stops on `StopIteration`). `Enumerator.new { \|y\| ... }` is a fiber-backed generator (`y << v`, `y.yield(v)`, and the bare `y.yield v` without parentheses, plus `#next` / `#peek` / `#rewind` / `#take` / `#first`, infinite generators work). `Enumerator::Lazy` over an int range (incl. endless) or int array fuses map/select/reject/filter/take_while chains terminated by `first(n)` / `to_a` / `force`. Chained block→`.to_a` forms (`each_slice(n).to_a`, `filter_map`, `map{}.to_a`) also work. |
| `Enumerable#each_entry` on a user class whose `#each` yields MULTIPLE values | yields them spread, as `#each` does, rather than packed into an array | on every builtin enumerable (Array/Hash/Range/Enumerator/Dir) `#each` yields one value per element, so `each_entry` is compiled as `each` and matches CRuby exactly. The difference only shows for a user `#each` that does `yield a, b`, where CRuby's `each_entry` hands the block `[a, b]`. Packing needs the yield arity of the user's `#each`, which is a static property of its body |
| `redo` in the block of an iterator whose emitter walks the body itself (a lazy pipeline's stages, `chunk_while`, `transform_values`, `Array.new`, ...) | refused at compile time, naming the line | `redo` re-runs the body in place, which needs a label after the block's setup. `each`, `times`, `upto`, `map`, `select`, `reject`, `find`, `flat_map`, `sum`, `inject`, `sort_by`, `uniq`, a comparator for `sort`, `min` or `max`, `bsearch`, `gsub`, `map.with_index`, the in-place filters, `map!`, `fill`, `product`, `tap`, `each_char`, `each_index`, a user `yield` and the other iterators that go through the shared body emitters place one; each remaining emitter would need the same. Compiled as a `continue`, it used to leave the block as `next` does |
| `StringIO#each_line` / `#each` / `#each_char` / `#each_byte` with NO block | `LocalJumpError`, where CRuby answers an `Enumerator` | the block forms are exact. Answering an Enumerator instead would make the method return either that or `self`, a union with no C slot. `io.readlines.each`, `io.read.each_char`, and `io.read.bytes` say the same thing and do have one |
| `StringIO#readpartial` / `#sysread` / `#read_nonblock` with a buffer argument | the data comes back as the result, but the caller's own buffer variable is not changed | the methods are plain Ruby in the stringio package, and a String parameter a package class's method changes is not yet passed by reference the way a user class's is. Through a value that may be a File or a StringIO (`def rd(io) = io.readpartial(n)` called with both) the three methods raise NoMethodError: that receiver takes the built-in IO arms, which don't see a package's plain-Ruby methods |
| `IO::Buffer` | the full in-memory API, CRuby-faithful: `new` (INTERNAL/MAPPED flags), `get_value`/`set_value`/`get_values`/`set_values` over all 18 type symbols (little/big-endian, `u64` round-trips Bignums under `--int-overflow=promote`), `get_string`/`set_string` (NUL-safe binary), `resize`/`clear`/`copy`/`size`, `slice` (live views, safe across a source `resize`), `transfer`/`free`/`dup`, `<=>`/`==`, `hexdump`/`inspect`/`to_s`, the predicates, the tiling bitwise family (`&` `\|` `^` `~` and `and!`/`or!`/`xor!`/`not!`), `locked`, `IO::Buffer.for(string)`/.string/.size_of, the CRuby exception classes (`IO::Buffer::AccessError` etc.), and the IO integration: `#read`/`#write`/`#pread`/`#pwrite` against an IO (one syscall each, answering the count, 0 at EOF or -errno; a blocking read on a socket or pipe parks the green thread, and the buffer is locked for the duration) and `IO::Buffer.map(file, size, offset, flags)` as an mmap view (READONLY / SHARED / PRIVATE; munmap'd by the finalizer; `resize` refused as for EXTERNAL). Passed to an `ffi_func` pointer argument, a buffer hands C its base address for the call ([FFI.md](FFI.md)). No `require` needed, as in CRuby. A literal type symbol compiles to a direct typed accessor (the wasm-runtime / binary-protocol hot path) | `#read` serves the bytes the IO's own stream already buffered before reading the descriptor (a `getc` followed by a `read` sees the next bytes); CRuby's reads the descriptor directly and can skip what its buffer holds. Three more deliberate divergences: `IO::Buffer.for(string)` copies (Spinel strings are immutable, so unobservable) and its write-through BLOCK form raises `NotImplementedError`; `each`/`each_byte`/`values` are block-form only (no Enumerator, as with StringIO); `get_string`'s third (encoding) argument is not accepted |
| The value of `super` in `initialize` (`c = super`, `super.frozen?`, `c = if f then super else [] end`) when the parent's `initialize` is the program's own | refused at compile time, naming the line | an `initialize` is compiled to return nothing, since `new` drops its value; one whose value a subclass's `super` asks for would return its last value instead |
| `Array#hash` (and arrays as hash keys) | unsupported | a builtin is additive, but array *keys* need the fundamental key-dispatch above |
| `IO.popen` | refused at compile time, naming it | the bundled `open3` package's `Open3.capture2` / `capture3` (with `stdin_data:`) and `Process.spawn` with pipes cover what it is used for; the method itself is a stream held open over a child, which the open3 package does not model yet |
| Sockets | TCP / UDP / UNIX-domain, as IO handles -- see below | additive: each missing class and method is its own runtime binding |
| Passing data through a named pipe (FIFO) between two threads, **on macOS** | the reader gets nothing and the program hangs; Linux answers what CRuby answers | not the open, which is what #4394 was about, and not any change since: a reader and a writer exchanging three lines through one `mkfifo` path fails on macOS against a tree with no runtime change at all (#4406), so it is the readiness path a FIFO descriptor reaches once both ends exist. A pipe (`IO.pipe`) or a UNIX-domain socket carries the same traffic and works on both. Opening a FIFO no longer stalls the other green threads on either platform |
| `class LoadError` / `class NameError` / `class Exception` … reopenings of a builtin exception class (activesupport's `core_ext/load_error.rb`, `core_ext/name_error.rb`, `Exception#as_json`) | supported | the class stays the runtime's: `raise LoadError, msg`, `LoadError.new(msg)`, `rescue LoadError => e`, `is_a?` and `e.class` behave as before the reopening (they used to build a shadowing user class, so `raise LoadError, msg` was a TypeError). The added methods take the runtime exception as self and are reached on a rescued or constructed exception and on a user subclass's instances; a bare `message` / `key` / `name` / `path` inside one is the exception's own. When several reopenings define one name (`Exception#brief` and `KeyError#brief`), a base-typed receiver is told apart by its runtime class, most-derived first in declaration order; a poly (run-time-typed) receiver does not see these methods yet |
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
- **`String#then` / `#yield_self` with a callable block** -- a non-literal
  `&proc` is refused in both builds: the builtin emitter cannot invoke it while
  preserving its returned String's identity. Use a literal block. Literal blocks,
  symbol-to-proc forms that desugar to one, and user-owned methods still compile.

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

That holds in every `--int-overflow` mode (#7612): an Integer that is exactly
-2**63 -- `-9223372036854775807 - 1` computed without overflowing, a wrapping
`+ - *`, `~0x7fffffffffffffff`, a bitboard's `1 << 63` in wrap mode -- is the
sentinel's word, so where it is printed, tested for nil or truthiness, or
computed on it can read as `nil`, and no mode raises for it. **Not where the
compiler can see that no nil reaches the value:** a literal, the result of an
arithmetic or bitwise operator, `||` / `&&` / `?:` of such values, and a local
whose every write is one of those are plain Integers, and printing,
interpolating, `to_s` / `inspect` / `to_i` / `to_f`, `nil?`, `zero?` /
`even?` / `positive?` and the like, and a condition test them for nothing
(test/int_min_plain_locals.rb, in raise and wrap). An `+ - * / %` whose two
operands are plain uses the helper without the nil test of its operands
(`sp_int_add_nn` ...), so -2**63 is a number there in raise mode too, and the
test is not paid. Parameters, return values,
instance variables and elements of Arrays and Hashes are not seen that way
yet: a bitboard that travels through them still reads as `nil` at the
sentinel's word. This is deliberate: checking the result of every `+ - *` costs
10 to 37% of the run time on integer-heavy programs (measured on `bm_tarai`,
`bm_tak`, `bm_sudoku`, `bm_structaref` and `bm_throw`), and an error where the
word was a real nil (a nil the analysis did not mark) would be wrong the other
way. The literal `-9223372036854775808` is not affected, and neither is a
value that travels as a Bignum (promote mode). A program that really uses
-2**63 as a number (a sentinel of its own, a hash seed) differs from CRuby.

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

#### A String Range keeps copies of its endpoints

A Range of Strings holds its two endpoints by value, as copies. A change in
place to the String an endpoint was made from (`<<`, a `!` method) is not
seen through `begin`, `end`, `first` or `last`, in the default build and
under `--share-strings` alike:

```ruby
first = String.new("aa")
r = (first.."zz")
first << "x"
p r.begin       # "aax" in CRuby, "aa" in Spinel
```

Sharing it needs a Range whose endpoints are shared String handles; that is
part of the `--share-strings` work (#7721).

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

#### Some ENV result routes cannot carry a shared String yet

With `--share-strings`, `ENV.store` and `ENV.[]=` return their value
argument's handle while the environment keeps a copy. A shared String
stored through a boxed value is refused when another live name could
observe a copy of its plain bytes. Fresh results and fresh values passed
at a local's only read are supported. The result route also refuses a
mutable key whose handle cannot be held while value evaluation can change
it; a plain value read does not require this refusal. A shared String
returned by an `ENV.delete` block is refused when the result route cannot
carry that handle. Delete's block parameter is the String key, and a block
that returns its own new String remains supported. `ENV.fetch` preserves
a default's available String handle. The snapshot
routes for `ENV.assoc` and `ENV.rassoc` likewise refuse an observable copy
of the supplied key or value.

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

With `--share-strings`, loading a native class such as `StringIO` does not
by itself invalidate a fresh String returned by a user method through a
boxed receiver. Native candidate entries with no Ruby method, reader, or
native binding for the called name or `method_missing` add no return path.
Real native bindings and readers remain conservative, and the existing
user-target and dynamic-hook checks still apply.

With `--share-strings`, a method that returns a fresh empty Hash directly
(`{}` or a blockless `Hash.new` with no default or a String/nil default) can
carry shared String values even when inference otherwise gives the return a
String-keyed, String-valued type. The promotion preserves the Hash and stored
String identities through callers and ivars. RBS-pinned returns and methods
with nonempty or alternate Hash producers keep their existing inferred types.

With `--share-strings`, a boxed call may also compose an exact String builtin
row with same-named user-method returns when the settled dispatch plan accounts
for both. For example, a blockless two-argument `String#sub` can return a fresh
String through its String arm while a same-named user method returns `nil`;
matched and unmatched substitutions both keep their String result separate
from the receiver. A builtin row's argument peeks also cannot discard values
retained by its same-named user arm. The analysis still joins every reachable
user return, so a user arm that returns an existing String retains that alias
and any required mutation refusal. Ambiguous builtin or dynamic dispatch
remains conservative. If the program declares singleton readers or writers,
this composition precision is also disabled: a user method can reach class-side
publication through a helper even when that accessor is absent from the call's
dispatch plan, and those holder edges are not fully represented here. Such
programs retain the previous boxed-container effects.

Not yet shared:

- String `self` handed to a mutating block through `yield` or `block.call` in the default build. Build with `--share-strings` to keep receiver mutations through this route. Read-only blocks and fresh String returns work in both builds;
- a String variable in a splatted Hash literal (`**{ k: v }`) at a dynamic call or yield whose key binds an appending keyword parameter;

- a String variable in an Array literal feeding an appended nested multiple-assignment target;

- a bare instance-variable argument written from a local, handed to an appending parameter through a call or `super`, unless the instance variable is already a shared handle;

- a repeated keyword whose later value is a String variable bound to an appending parameter, unless the value is already passed as a shared handle;

- through `Thread.new` or `Fiber#resume`, a String variable handed to a block parameter that appends to it, unless its read already hands over the shared handle or the local is read only as that argument;
- through a Hash's value block (`each_value`, `each`, `each_pair`, or an element iterator over `values`, `values_at` or `fetch_values`), a stored String variable when the value parameter appends to it;
- through a Hash's `[key, value]` pairs (`h.to_a`, `h.first`, `h.min_by { }`, `k, v = h.first`, an iterator over them), a String value that is then mutated;
- through `yield` into a capture-wrapper block, a String variable whose captured parameter appends to it without already being the shared handle, including a splatted yield;
- through an Array's chained index into an appending block;

- through a retained `scrub!` result that is appended to; `scrub!` with a block is also refused because the block would be ignored;
- through a container element, a String a boxed local holds (`s = [+"xy", 1][k]`) stored into an Array, a Hash, an instance variable's or a global's Array and mutated in place through an element read or an iterator's block parameter (`[s][0].prepend(x)`, `[s].each { |e| e << x }`);
- through a literal's element, a local bound from an element read of an Array or Hash literal holding a String variable (`t = [s][0]`, `t = [s].first`), when the local is mutated in place and the variable is read again;
- through a global variable: a String variable assigned from a global (`t = $g`, `t = $g.to_s`) or to one (`$g = s`, `$h = $g`), when one name is mutated in place and the other is read;
- through a method that returns its String parameter (`def id(x) = x`, also `x.itself` or a bang method on it), when the result or the variable passed in is mutated in place and the other is read (a variable passed in through the caller's own parameter counts: the caller reads it again). A reopened String method returning an omitted parameter whose default is `self` hands on a copy of the receiver in the default build and is not refused (`--share-strings` shares it);
- through a bang method's result, which is its receiver (`r = s.strip!`, `r = s.strip! || s`), when the result is mutated in place through a variable and the receiver is read, by its own name or, for a local that shares its String, through the other one (`t = s; r = t.strip!`); a mutator straight on the result (`s.sub!(a, b) << x`) reaches the receiver;
- through the result of `bytesplice`, `append_as_bytes`, or `concat` or `prepend` with other than one argument, which is its receiver too (`r = s.concat(a, b)`), when the result is mutated in place through a local, a global or an instance variable of the top level and the receiver is read, or when a method returns its parameter through one of them. A class's instance variable that keeps the result is left alone: an append through it reaches an instance-variable receiver in some programs (`@r = @s.concat(a, b)` where a parameter set `@s`) and does not build in others, and most other changes through either name are lost. A mutator straight on the result of `bytesplice` or `append_as_bytes` (`s.bytesplice(0, 1, "Z") << x`) does not reach the receiver. Neither of these is refused yet;
- through `to_s` (or another call answering its String receiver) under a mutator whose argument reassigns the variable, when the variable is a String handle (`e.to_s << (e = +"b")`);
- into an Array, a String variable added by `insert`, `prepend`, a `concat` of a literal, the later push of a chain (`a << t << s`) or a push into a global Array, or as the value of `s << x`, of `+s` (s itself unless s is frozen; a parameter is refused even when every caller passes a frozen literal) or of a reader method (`a << sb` with `def sb = @s`), when the variable or the element is then mutated in place;
- into an Array or a Hash, the result of a method that returns its String parameter, stored straight (`a << id(s)`, `a[0] = id(s)`, `a = [id(s)]`, `h = { k: id(s) }`), when the variable is mutated in place and the container is read, or an element is mutated in place and the variable is read. A constructor argument (`Box.new(id(s))`) and a literal that is not assigned to a variable are still copies and are not refused;
- through a reader on a boxed receiver (a Struct or Data member, an `attr_reader`, `def m = @iv`), a String read into a local that is mutated in place while the receiver is read again (a local only queried with String's own methods and then rebound to a copy with String's own `dup`, `s = s.dup`, before the change compiles: the change is the copy's);
- into a container, a String held by a block parameter no element iterator binds (a proc's, a lambda's, a yielding method's block, `each_char`'s, `scan`'s), when an element is then mutated in place;
- through `scan`'s block parameter, a match the block keeps and mutates in place (it did not build);
- through a global variable's Array, a String element mutated in place through the Array (`$b.each { |y| y << x }`, `$b[0] << x`);
- into an Array or Hash a caller passes a method, a String the method stores (`def keep(a) = (a << n)`), when the caller mutates the element in place;

- through an ivar's or a call's Array, a fresh Array literal, a narrowed boxed String element, or a fresh String's `tap`, into an appending block or parameter;
- through a parameter of a reopened builtin's method that the method yields on or passes to its `&block` (`class String; def po(a) = yield(a); end`, then `s.po(u) { |v| v << x }`), into an appending block: the block appends to a copy in the default build, and this is not refused (`--share-strings` shares it);

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
  class value's `new`, one held by an instance variable;
- through a `Method` bound to one of the String's own in-place mutators
  (`s.method(:<<)`, `s.method(:concat)`, `s.method(:upcase!)`, their
  `to_proc` and `&s.method(:<<)`): the Method is bound to the String's
  value, so `.method` itself is refused, naming the line. Call the
  mutator on the String, or wrap it in a block (`->(x) { s << x }`).

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

The block of a lazy stage (`[s].lazy.map { |x| x << "!" }`, and `select`,
`take_while` and the other stages up to the first `map`, past a `with_index`
too) is handed a boxed
copy of the element, so a block that changes its String element in place
is refused as well, naming the line. Drop the `.lazy` (the eager form
shares the String), or return a new String (`x + "!"`).

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

A note on identity: equal frozen literals are one object, as in CRuby
with frozen string literals. `"abc".equal?("abc")` is `true`, the same
text in two methods (or in two parts of a `--jobs=N` split build) is the
same object, an adjacent-literal fold (`"ab" "c"`) is the object the plain
`"abc"` is, and a literal in a loop yields one object on every pass. An
interpolated string (`"#{x}"`) is built anew each time, in CRuby too.

What still differs is the run-time intern table behind `String#-@` /
`dedup`. CRuby puts every frozen literal in it when the code is loaded, so
`-("ab" + "c")` returns the literal `"abc"` itself. Spinel's table holds
only what `-@` / `dedup` has been called on: two run-time strings dedup to
one object, and `-"abc"` returns the literal when the literal is the first
of its content to be deduped, but a run-time string deduped before that is
not the literal (`(-("ab" + "c")).equal?("abc")` is `false`), and the
literal's own `-@` then returns that earlier object. `str.dup.freeze` is
never deduplicated, in CRuby either.

`Symbol#to_s` and `#id2name` answer a new String on every call in CRuby, so
`:abc.to_s.equal?(:abc.to_s)` is `false`. Spinel keeps one chilled String per
symbol and answers it each time, so that is `true`. The value is the same,
and so is every mutation: the String is chilled, so `s = :abc.to_s; t = +s`
copies, and `s << "x"` makes `s` its own String and leaves the next `to_s`
alone (`:abc.to_s` is still `"abc"`). Only the identity of two `to_s` results
differs; a new String per call would cost an allocation at every symbol read,
which programs that build names from symbols do in loops.

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

#### `defined?(@ivar)` and the other reads of ivar presence

CRuby answers `defined?(@ivar)` from the object's runtime state: `nil` until
the instance variable is first assigned, `"instance-variable"` after -- which
is what makes it usable as a memoization guard for falsy values
(`return @x if defined?(@x)`). `instance_variables`,
`instance_variable_defined?`, the default `inspect` and `Marshal.dump` read
the same state, and `remove_instance_variable` clears it.

Spinel's instance variables are C struct fields, pre-filled with their type's
nil representation. A field `initialize` always assigns before anything can
look at the object is always present, and one every write of which stores a
non-nil value is present exactly when it is not nil. A field neither answers
for keeps an explicit assigned flag beside it, set by each store after its
value is computed and cleared by `remove_instance_variable`, in the classes
such a read reaches (the receiver's class and those below it, through a
container or another object's ivar for `inspect` and `Marshal.dump`). So
these work as in CRuby:

```ruby
class Foo
  def compute = 42
  def foo
    return @foo if defined?(@foo)
    @foo = compute
  end
end
f = Foo.new
p f.foo, f.foo                    # 42, 42

class Bar
  def initialize(v) = (@v = v if v)
  def has? = defined?(@v)
end
p Bar.new(nil).has?               # nil
```

The flag cannot follow an ivar that is a `for` loop's or a `rescue => @x`
target, that a writer reached by name (`send(:x=, v)`) or a bitwise
op-write through a writer (`o.x |= v`) assigns, or that a class whose ivars
the compiler writes itself holds; nor any ivar once the program calls
`instance_variable_set` with a computed name. Such an ivar, and one whose reads reach it only
through a boxed value the analysis cannot bound, is reported as assigned
from the start, as before.

CRuby lists an object's ivars in the order they were first assigned, which
differs between the objects of a class that assigns some of them on some
paths only. In a class such a read reaches, whose ivars `initialize` does
not all assign unconditionally, every ivar keeps beside it the rank of its
first assignment (a store to an assigned ivar keeps its rank, a removal
clears it and a later assignment ranks it last), and `instance_variables`,
the default `inspect` and `Marshal.dump` list by it; `dup` and `clone` copy
the ranks and `Marshal.load` assigns in the order the stream gives.

A class whose `initialize` assigns every ivar unconditionally lists them in
the order it does (a `super` standing for the parent's own, or for an included
module's `initialize` where the class has one), provided nothing before the
last of those assignments could assign an ivar first: the statements up to it
must be plain ivar assignments, multiple assignments or a `super` without a
block, whose values and arguments (and the defaults of the optional parameters
and keywords) call nothing of the object's own (no call on self or without a
receiver, no block, `yield`, `super` or ivar write), and not inside a `begin`
with a `rescue` or `ensure`. This order also applies to exception classes and
value types, which the ranking leaves out. Otherwise, for a class with an ivar
the flag cannot follow, for a Struct, and in a program that calls `allocate` or
`Marshal.load` (their objects run no `initialize`), the ivars list in the order
the class lays them out, which differs from CRuby's when a method names one
before `initialize` assigns it, `initialize` calls a method that assigns one,
or it assigns one conditionally first. A listing through a receiver typed as a
class whose subclasses list other ivars, or the same in another order, lists
as the object's own class does.

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

`equal?` on strings is pointer identity. Equal frozen literals are one
object, as in CRuby (see the identity note under the frozen-string-literal
section): `"x".equal?("x")` is `true`, and re-evaluating a literal (one in
a loop) yields the same object. Everything else about identity is
truthful: `s.freeze.equal?(s)` is `true` (freeze marks in place), aliasing
compares equal, `-str` dedups interned content to one object (but not
always to the literal of that content, as noted there), and
distinct-valued strings compare `false`.

#### `defined?`

`defined?` combines compile-time resolution with runtime checks. Local
variables, known constants, implicit method calls, assignments and literals
usually resolve to a fixed label. Global-variable presence is inferred from
writes in the program. Class variables use a runtime assignment flag, and
calls with a receiver can check its value and whether it answers the method.
Instance variables use the static or runtime paths described above.

Examples compared with CRuby, with the referenced constant and method defined
and a block supplied for `yield`:

| Operand | CRuby | Spinel |
| --- | --- | --- |
| `Foo::Bar` (constant path) | `"constant"` | `"constant"` |
| `puts` (built-in / Kernel method) | `"method"` | `"method"` |
| `obj.meth` (public method with a receiver) | `"method"` | `"method"` |
| `1 + 1` (operator = method call) | `"method"` | `"method"` |
| `{a: 1}`, `1..3` (hash/range expressions) | `"expression"` | `"expression"` |
| `x = 1` (assignment) | `"assignment"` | `"assignment"` |
| `yield` | `"yield"` | `"yield"` |
| `super` in a subclass method with a parent implementation | `"super"` | `nil` |
| Multi-encoding strings | Strings are UTF-8 or ASCII-8BIT, and one rule says which: a string is ASCII-8BIT when the program ASKED FOR BYTES (`pack`, `String#b`, `Marshal.dump`, `Random#bytes`, `binread`, `unpack`'s byte directives, `force_encoding` naming it). Everything else is UTF-8, including what CRuby calls US-ASCII (see below). A string carries one bit, not an encoding object; `#length`, `#[]`, `#inspect`, `#encoding` and the comparisons read it. A compiled binary's string paths (indexing, regexp, hashing) assume the two share a byte representation, and that assumption is load-bearing for their performance | write UTF-8; transcode at the boundary before the data enters the program |

The static instance-variable path can report `"instance-variable"` before a
write on the particular receiver; see the memoization examples above. A class
variable instead reports `nil` before assignment and `"class variable"` after
assignment, including when the assigned value is nil.

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
| A synthesized `attr_writer` inherited by sibling user classes and called through a boxed receiver | works when the candidate classes share one effective attribute family; inherited writer copies use the common ancestor's slot. An explicit writer override and an unrelated same-named attribute stay separate; Spinel does not infer their ivar types from the writer name alone. |

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

String line iteration with a nil separator yields the receiver itself. A block
that mutates that String while another name observes it is refused because the
line iterator cannot bind the receiver handle. Array#fill block results use
shared handles under `--share-strings`; the default build refuses an observed
mutation through a retained String result.
