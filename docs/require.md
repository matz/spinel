# `require` in Spinel

Spinel is a *subset* of Ruby: a program that compiles and runs under Spinel
should behave the same under CRuby. One place Spinel used to be a *superset*
instead was `require`. Some stdlib that CRuby gates behind a `require` --
`StringIO`, `IO#winsize`, and friends -- was always available in Spinel, so code
that forgot the `require` ran under Spinel but raised `NameError` /
`NoMethodError` under CRuby. The **require-gate** closes that gap.

This document describes how `require` works today, which stdlib needs which
`require`, and how features are resolved. The planned package system is sketched
under [Planned](#planned) at the end.

## `require` vs `require_relative`

They are different mechanisms and stay distinct:

| form | resolves to | when |
|---|---|---|
| `require_relative "path"` | a project-local file, spliced into the program | always; a missing file is a **compile error** |
| `require "name"` | a named **feature** (bundled stdlib, native, or -- planned -- a package) | the feature must exist, else a **compile error** |
| `Kernel.require "name"` / `::Kernel.require "name"` | the same as the bare `require` | as above; any other receiver (`obj.require`) is an ordinary method call |

`require` is *not* a runtime file load. Spinel is a whole-program
ahead-of-time compiler, so every `require` is known at compile time and there is
no `$LOAD_PATH` to mutate at runtime. A `require` that names something Spinel
cannot provide is reported when you compile, not when you run.

## The require-gate

The require-gate makes require-gated stdlib unavailable unless you `require` it,
matching CRuby. It is currently **opt-in**: pass `--require-gate`, or set the
environment variable `SPINEL_REQUIRE_GATE=1`, when compiling. With it off (the
default today) the old always-available behaviour is preserved. The gate is
expected to become the default at a future release boundary.

```sh
spinel --require-gate myprogram.rb
SPINEL_REQUIRE_GATE=1 spinel myprogram.rb   # the same switch
```

`spin build` always compiles with the gate on: inside a resolved dependency set
the universe is known, so a `require` that resolves to nothing is a bug rather
than something to warn past. `spin flags` hands the flag to a build driven from
outside spin, which is why it has a flag spelling at all -- an environment
assignment cannot ride inside a flag string.

### Require-gated stdlib

These are provided by Spinel but, like CRuby, only after their `require`:

| `require` | what it enables | without the require (gate on) |
|---|---|---|
| `require "stringio"` | `StringIO` | uninitialized constant |
| `require "strscan"` | `StringScanner` | uninitialized constant |
| `require "json"` | `JSON.generate`, `JSON.dump` | uninitialized constant |
| `require "csv"` | `CSV` (with `CSV::Row` / `CSV::Table`) | uninitialized constant |
| `require "monitor"` | `Monitor` (`#synchronize`) | `NameError` (uninitialized constant) |
| `require "socket"` | `TCPServer`, `TCPSocket`, `UDPSocket`, `UNIXServer`, `UNIXSocket`, `Socket`, `Addrinfo` | `NameError` (uninitialized constant) |
| `require "pathname"` | `Pathname` | uninitialized constant |
| `require "pathname"` | `Pathname()` (the `Kernel` conversion method) | `NoMethodError` |
| `require "io/console"` | `IO#winsize` | `NoMethodError` |
| `require "time"` | `Time#iso8601` | `NoMethodError` |
| `require "bigdecimal"` | `BigDecimal`, `BigDecimal()` (a minimal subset: see `packages/bigdecimal/bigdecimal.rb`) | `NoMethodError` (`BigDecimal()`) |

`socket` is the strictest of these: its `require` is mandatory even with the
gate **off**, because CRuby itself only defines `TCPServer` / `TCPSocket` after
the require, and growing the constants unconditionally would diverge from
CRuby's `NameError`. What the two classes actually cover is in
[limitations.md](limitations.md#sockets).

A library loads itself and nothing else. CRuby's stdlib files often require
others as an implementation detail (`csv` pulls in `stringio` there, so a
program that requires csv can name `StringIO` without saying so), and Spinel's
version of the same library has no reason to make the same internal choice.
Require what the program actually uses.

Two shapes, which set what the failure looks like:

- A feature that **defines a class/module** (`stringio`, `strscan`, `json`,
  `monitor`): without the `require` the constant is undefined.
- A feature that **extends a core class** (`io/console` adds `IO#winsize`,
  `time` adds `Time#iso8601`): without the `require` the method is undefined.
  The rest of `IO` and `Time` are core and always available -- only the gated
  method needs the `require`.

```ruby
# gate on, no require:
StringIO.new("x")     # error: uninitialized constant StringIO
STDOUT.winsize        # error: undefined method 'winsize'

# correct:
require "stringio"
require "io/console"
StringIO.new("x").read
STDOUT.winsize
```

### Unsatisfiable requires

A `require` that Spinel cannot satisfy at all -- a stdlib Spinel does not
implement (`date`, `pp`, `securerandom`, ...) or an unknown name -- is a compile
error under the gate, the same as a missing `require_relative`:

```
spinel: cannot load such file -- date (require "date")
```

This is honest about the subset: if Spinel cannot provide `date`, the program
genuinely cannot run, and you learn it when you compile rather than via a
confusing failure later. With the gate off this is a warning instead.

A few `require`s name a capability Spinel already provides as core, and are
**tolerated** as a no-op (modern CRuby treats them as already-loaded too):

- `require "thread"` (Thread, Mutex, Queue are core)
- `require "enumerator"` (Enumerator is core)
- `require "fiber"` (Fiber is core)

### Pre-installed packages (the carved-out stdlib)

Some stdlib ships with Spinel as Ruby source and is spliced when required --
`set`, `forwardable`, `optparse`, `erb`, `csv`, `pathname`, `digest`, `base64`,
`fileutils`, `tmpdir`, `zlib`, `open3` (the capture forms), `pty` (`PTY.spawn`; not on
wasm32-wasi, which has no fork), `fiddle` (where libffi is,
like `ffi`),
`benchmark` (CRuby's own gem, unmodified), `shellwords` (likewise), `find` (likewise, but for one line; see its note),
`fileutils`, `tmpdir`, `zlib`, `logger` (a compact implementation of its API),
`benchmark` (CRuby's own gem, unmodified),
`bigdecimal` (a minimal subset, see `packages/bigdecimal/bigdecimal.rb`)
(plus the `stringio`/`strscan`/`json` marker shims for their C-backed
features). `net/http` and `uri` are there, and so
is `openssl` -- the one that is conditional: it is glue over the system libssl, so it exists only where those
headers did at build time, and `require "openssl"` is otherwise the
unsatisfiable require it is for any library Spinel does not carry. Where the
build found a keg-only OpenSSL (Homebrew's `openssl@3`), the compiler records
that library directory and links a program's `-lssl -lcrypto` against it, so
no `LIBRARY_PATH` is needed after `make`. Each lives as an ordinary
spinelgem under `packages/<name>/` beside the compiler (`packages/set/set.rb` with
its `spin.toml`); `lib/` holds only the C runtime. The `require` pulls in
the package's file like any other package -- pre-installed just means no fetch.

Two of these are also spliced **implicitly**, because CRuby provides them with
no require at all: a program that references `Set` (or calls `.to_set`)
anywhere, in the entry file or in a file it requires, gets `require "set"`
prepended, and one that references `IO::Buffer` gets
`require "io/buffer"` (packages/io) the same way. Writing the require
explicitly is fine too -- the splice only fills it in when absent.

## Providing your own feature

You can provide a feature yourself and have `require "name"` resolve it, through
the same mechanism as bundled stdlib. Pass `-I <dir>` (like `ruby -I`) to add a
feature search root, then a `require` resolves against it:

```sh
spinel -I mylibs main.rb
```

The `-I` roots are searched before the packages bundled with the compiler (`packages/`), so a project's own package of the same name as a bundled one (an `openssl` of its own, say) is the one a `require` reaches. The compiler's `lib/` comes first and is not shadowed.

A feature name is a path, looked up in each `-I` root in two forms:

- **single file** -- `require "thing"` → `mylibs/thing.rb` (the CRuby form);
- **colocated directory** -- `require "thing"` → `mylibs/thing/thing.rb`, so a
  feature's sources (`.rb`, later its `.c`/`.rbs`) share one directory.
  `require "my/thing"` → `mylibs/my/thing.rb` or `mylibs/my/thing/thing.rb`.

Pure Ruby needs only the `.rb`: it is spliced into the whole-program compile and
Spinel infers types from it like your own code, so no manifest or `.rbs` is
required (an `.rbs` is optional, to pin the public surface). A `require` that no
root satisfies is the compile error from the previous section.

**A spin package is already in the colocated form.** A package named `curses`
lives at `<pkgs>/curses/curses.rb`, so `-I <pkgs>` resolves `require "curses"`
against it -- with no project file and no `spin` involved -- and one root serves
every package under it. Its carried C reaches the link line through the
compiler's repeatable `--link`:

```sh
spinel -I spin/packages --link ~/.cache/spin/native/curses-0.1.0-cc/sp_curses.o app.rb
```

The root has to be the directory the package sits *in*. `-I .` at a repository
root whose packages live under `spin/packages/` looks for `./curses.rb` and
`./curses/curses.rb`, finds neither, and the `require` is ignored with a
warning (or refused, under the gate). Working out which roots and which objects
a dependency set implies is what [`spin flags`](spin.md#building-outside-spin)
does for you.

### Planned

- C in a feature is reached through [FFI](FFI.md) today (wrapping an existing
  library); a planned in-TU mode will let a feature's own C be inlined without a
  link boundary.
- A default vendored root and a project manifest will sit on top of `-I`, so
  packages resolve without passing `-I` by hand.
- Feature directories (a `-I` root, a vendored package) are read-only inputs:
  Spinel never writes build artifacts next to a feature's sources. A C-backed
  feature's compiled objects go to a separate build cache, so a feature tree can
  be shared or mounted read-only.
- Spinel's own require-gated stdlib will eventually be carved out of the
  compiler into ordinary feature packages, resolved exactly like a third-party
  one.
