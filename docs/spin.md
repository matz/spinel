# spin -- projects and packages

`spin` is Spinel's project tool: it scaffolds a project, resolves
dependencies (spin packages), and drives the compiler so you never write a
Makefile or a `spinel -I ...` line by hand. If you know cargo or mix, you
know the shape. The design record lives in
[internals/spin.md](internals/spin.md); this page is how to use it.

`spin` ships beside the compiler: building the repo (`make`) produces
`bin/spin`, and `make install` installs it next to `spinel`.

## Starting an application

```sh
spin new myapp        # scaffold: spin.toml, bin/myapp.rb, test/, .gitignore
cd myapp
spin run              # compile bin/myapp.rb and run it
```

```
myapp/
  spin.toml            # the manifest (name, [dependencies])
  myapp.rb            # library code: require "myapp" resolves here
  myapp/              # subfeatures: require "myapp/util" -> myapp/util.rb
  bin/myapp.rb        # each bin/*.rb is an executable (a compile root)
  test/               # each test/*.rb is a test program
  build/              # disposable output (spin clean)
```

An application **is** a package: there is no separate project kind. Executables
live in `bin/` (one per file, `spin run <name>` when there are several).
Grow the app by putting shared code in `myapp.rb` / `myapp/*.rb` and
requiring it from `bin/`; more `bin/*.rb` files become more executables.
`spin init` writes a `spin.toml` into an existing directory instead of
scaffolding. When the CLI is ready for daily use, `spin install` builds it
and copies the executables to `~/.local/bin` (`$XDG_BIN_HOME` / `--prefix`
override; `--uninstall` removes them).

Everything in the package participates by extension, not by manifest lists:
`.rb` is source, `.rbs` is an optional type sidecar, `.c`/`.h` is carried
native code (below). `build/`, `vendor/`, `test/`, and `bin/` are the only
special directory names.

## Starting a library

```sh
spin new mylib --lib  # spin.toml with [package] name/version, mylib.rb, test/
cd mylib
```

A library is the same package shape minus `bin/`: there is nothing to `spin
build` or `spin run` -- a library is *exercised through its tests*:

```sh
cat > mylib.rb <<'RUBY'
module Mylib
  def self.shout(s) = s.upcase + "!"
end
RUBY
cat > test/shout_test.rb <<'RUBY'
require "mylib"
puts Mylib.shout("hi")   # HI!
RUBY
spin test                # runs it (against CRuby when no snapshot yet)
spin test --regen        # freeze the output as the .expected snapshot
```

While developing an application against your library, wire it up as a
live path dependency -- edits take effect on the next build, nothing is
pinned:

```sh
cd ../myapp
spin add mylib --path ../mylib
```

To share it, push the directory as a git repo (conventionally named
`spinel-mylib`; the gem *name* stays `mylib` because it is the `require`
string). Consumers then use it directly:

```sh
spin add mylib --git https://github.com/you/spinel-mylib
```

or, once it has releases, through [the index](#the-index). Publishing a
release is one command:

```sh
spin publish
```

It validates the release (committed and pushed, `[package] name`/`version`
present, the tree at the release commit carrying that same version, the
name not owned by another repo in the index), **runs `spin test` as a hard
gate**, then submits `packages/mylib.toml` with a `[[release]]` entry pinning
the full commit SHA: as a pull request to
[spin-index](https://github.com/matz/spin-index) when the `gh` CLI is
available, by printed instructions otherwise, or pushed directly with
`--direct` if you have index write access. From then on
`spin add mylib --version "~> 0.1"` works, and bumping `version` +
`spin publish` again is how you ship an update. Libraries do not commit a
`spin.lock`; version selection belongs to the consuming application.

### One version, one spelling

Versions are compared field by field as numbers, with the shorter side padded
with zeros, so `0.1` and `0.1.0` are the **same version** to every constraint --
and so are `2026.9.8` and `2026.09.08`. Publishing both would put one version
in the index twice and split the native object cache, whose directory name is
the version as written. `spin publish` refuses the second spelling and says
which one is already there. Pick a spelling and keep it.

## Dependencies

Declare dependencies in `spin.toml`; `spin` computes the compiler's `-I`
list from them. Three source forms:

```toml
[dependencies]
ansi  = { path = "../spinel-ansi" }              # local checkout
greet = { git = "https://github.com/x/spinel-greet" }  # git URL (+ ref = "...")
hello = "~> 1.1"                                 # index constraint (see below)
```

```sh
spin add ansi --path ../spinel-ansi   # edits spin.toml and relocks
spin add hello --version "~> 1.1"     # index form
spin remove ansi
spin list                             # resolved set: name, version, source
spin tree                             # nested view (--json on both)
```

Dependencies are transitive: each fetched package's own `[dependencies]` is
resolved too. Inside a project every `require` must resolve -- an
unsatisfiable `require` is a compile error naming the missing package, and
stdlib features need their `require` just like CRuby (`spin` compiles with
the require gate on; see [require.md](require.md)).

### The index

A bare `name = "constraint"` dependency is looked up in the index -- a git
repository (no server) mapping names to repos and releases:
<https://github.com/matz/spin-index>. Constraints are `"~> 1.2"`
(pessimistic), `">= 1.2.3"`, an exact version, or `"*"`.

Selection is MVS: `spin` picks the **lowest** release satisfying the
constraint, so a build without a lockfile is still deterministic;
`spin.lock` then pins the exact commit. `spin search [term]` lists index
entries. Set `SPIN_INDEX` to use another index (a `file://` URL works).

### The compiler's own version

`spinel --version` prints the release the build belongs to, then the build
revision in parentheses, then the C compiler:

```
spinel 2026.09.12 (112bae85) [gcc 13.3.0]
spinel 2026.09.12+7 (94b68d85) [gcc 13.3.0 (cc)]   # seven commits past that release
spinel unreleased (6b8ddcd8) [clang 18.1.3]        # before the first release
```

The compiler is named by what it is (its predefined macros), not by the name
it was invoked under; that name is added in parentheses when it differs, as
`cc` does above.

Releases are dated: `YYYY.MM.DD`, with `.N` appended for a second release on
the same day. The fields are fixed width because the calendar fixes them, so
the plain string order is the chronological order and no field has to be
padded to a width guessed in advance. The name says when a release was cut and
claims nothing about how finished it is; what a release promises is recorded
in `docs/limitations.md` and in the tests, not in the number.

The revision is git's own short form (as long as it needs to be unique in the
repository, seven digits at least); it is what tells two builds of one release
apart, and `spin` reads it from the parentheses as the toolchain identity for
its probe records. `+N` is not part of a release name: it marks a build N
commits past the release, and only a tag makes a release.

### What a release is allowed to say

A release name claims nothing about quality, so the question a version has to
answer is the other one: what has stopped being allowed to change.

`ruby tools/promise_diff.rb [FROM [TO]]` reports that between two commits --
FROM defaults to the latest release tag, TO to HEAD. It reads five places, all
from git alone, so it needs no build and works over ranges already in the past:
the answers tests pin (`test/*.expected`), the PASS rows the ruby/spec
retention gate keeps, the entries in `docs/limitations.md`, the `spin.toml`
fields spin actually reads, and the compiler flags `spinel` accepts. Breaks are
separated from additions, and a promise withdrawn on purpose -- a spec moved to
`REJECT-BYDESIGN`, a limitation newly stated -- is separated from both.

It is a survey and always exits 0. Whether a break is acceptable is a
judgement; the report exists to put it in front of someone.

What it cannot see is a promise nobody wrote down. Carried C was compiled by
presence for years, which was a contract for everyone with a `.c` in their tree
and was written nowhere (#4362). Those arrive as bug reports, and writing one
down is what puts it in the report from then on.

Index entries also carry **probe records** -- which compiler build a release
passed or failed its tests under (`spin publish` records a pass for your
build automatically; `spinel --version` prints the build revision). When
you depend on a release with a recorded failure, resolution warns before
fetching -- strongly when the failure was recorded against your exact
compiler build -- but never blocks: your own build is the final answer.

### spin.lock

`spin lock` (and `spin add`/`remove`) writes `spin.lock`: one `[lock.<name>]`
entry per dependency with the resolved version and, for git/index sources,
the full commit SHA. Commit it for applications. Resolution *verifies*
against the lock rather than reselecting; if you change a constraint so the
pinned version no longer satisfies it, the build warns and reselects, and
the next `spin lock` rewrites the pin.

### Offline and vendoring

Fetched packages live in a shared cache (`$XDG_CACHE_HOME/spin/packages/`),
keyed by the commit SHA. `spin vendor` copies the resolved tree into
`vendor/packages/` for hermetic builds; with `SPIN_OFFLINE=1`, resolution uses
only the cache and `vendor/` -- nothing touches the network.

## Tests

Each `test/*.rb` is one test program, compiled with the package's sources and
dependencies spliced in. Pass/fail is snapshot-based:

```sh
spin test                 # run all tests
spin test smoke_test.rb   # one test
spin test --regen         # refresh .expected snapshots from CRuby
```

A committed `test/<name>.rb.expected` is diffed against the run's stdout.
With no snapshot, the same file runs under `ruby` and the outputs are
diffed directly -- the test doubles as a CRuby-parity check. A non-zero
exit or a diff fails, so plain assert-and-raise style works.

## Native C in a package

Drop `.c`/`.h` files anywhere in the package tree and bind them with the
[FFI declarations](FFI.md):

```ruby
# fast.rb
module Fast
  ffi_func :fast_quad, [:int], :int
end
```

```c
/* fast_ext.c */
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
```

The C a package carries is the C its manifest names:

```toml
[package]
name = "fast"
sources = ["fast_ext.c"]
```

`spin` compiles each named `.c` once into a shared cache keyed by
(package, version, toolchain) -- set `CC` to choose the compiler -- and links the
objects into every dependent build. External libraries use the existing
`ffi_lib` declaration and need no manifest entry.

### Declaring the C a package carries

Both halves of a package are declared. A `.rb` enters the build by being
required; a `.c` enters by being listed in `sources`. Without the key a package
carries no C at all, so a program of your own sitting beside the Ruby -- or a
scratch file left there while debugging -- is not compiled and cannot collide
with the generated `main` at link time.

Entries are paths relative to the package root and may be globs:

```toml
sources = ["native/*.c", "vendor/lib/**/*.c"]
```

`sources = ["*.c"]` is allowed and means what carrying C used to mean: compile
whatever `.c` is there. It is the author's call, and it picks scratch files back
up.

A `.c` in the tree that no entry names is reported and skipped, rather than
skipped quietly -- a forgotten entry would otherwise surface as an undefined
symbol at link, which names a symbol instead of the file. An entry that matches
nothing is reported for the same reason.

One kind of `.c` is never compiled however it is declared: a file spinel itself
emitted. `spinel app.rb -c -o out.c` writes a translation unit that defines
`main` and, through the compiler's internal header, its own copy of the runtime,
so compiling it as carried C collides with the real program on both. spin
recognises its own output by the banner on the first line, leaves it out even
when a glob reached it, and says which file it left out -- that file may be
sitting on top of a source of the same name it overwrote, in which case the
source is gone and needs restoring.

### Excluding C the declaration reached

`exclude` subtracts from what `sources` named, which is what a glob needs:

```toml
[package]
name = "myapp"
sources = ["*.c"]
exclude = ["standalone_c_app.c", "cbits"]
```

Globs are relative to the package root; naming a directory prunes all of it.
`exclude` covers native sources only -- `.rb` needs no entry, since nothing
compiles it unless something requires it, and an excluded `.h` is still on the
include path for the C that is compiled. An application scaffolded by
`spin new` has no `[package]` table; add one to use either field.

## Choosing an allocator

A program that runs for a second and exits spends no meaningful time in
`malloc`. A server does: it allocates for every request for as long as it is
up, and on that shape the allocator is a measurable fraction of the whole
profile. `allocator` names the one this program wants:

```toml
[package]
allocator = "jemalloc"
```

Anything `-l<name>` can link is accepted, and it becomes an ordinary link
input. `"system"` and the absent key both mean the platform's own allocator,
which is the default and stays the default: spinel compiles batch programs and
benchmarks as readily as servers, and the right allocator is a property of the
program rather than of the language. Only the manifest knows which kind of
program this is. An application scaffolded by `spin new` has no `[package]`
table; add one to use the field.

The field applies to the executables *this* package produces. A dependent
never compiles a dependency's `bin/`, so a dependency's `allocator` does not
reach your program: the one that applies is the one in the manifest you are
building. A process has a single allocator, and choosing it is not a decision
a library makes for everyone who depends on it.

The library has to be linkable at build time, which on Debian and Ubuntu means
the development package (`libjemalloc-dev`) and not just the runtime one
(`libjemalloc2`). Asking for an allocator that is not installed fails the
build:

```console
$ spin build
/usr/bin/ld: cannot find -ljemalloc: No such file or directory
```

That is deliberate. The manifest states what the program needs, and an unmet
statement should fail the way an unresolvable dependency does -- quietly
building something slower than what was asked for is how one machine's binary
comes to differ from another's without anyone noticing.

Installed but outside the linker's default directories is a different case:
Homebrew on Apple Silicon puts jemalloc in `/opt/homebrew/lib`, which Apple's
linker does not search. When `pkg-config` knows the library, `spin` puts the
directories it reports on `LIBRARY_PATH` for the build, so such a library links
without any setup. `spin pack` writes no host path into what it packs; the
build host's own linker and `LIBRARY_PATH` find it there.

On two Rails-derived applications compiled by spinel, `"jemalloc"` was worth
+58% on one OS worker and +25% on twelve for a 420 KB page, and +22% and +59%
for a 6 KB one. It is the same reason Rails ships jemalloc in its production
image.

## Building outside spin

`spin build` owns the tree it sits in. When the build is driven from somewhere
else -- a Makefile that also builds a C program, a repository whose layout is
not spin's to arrange -- `spin flags` hands over instead of taking over. It
resolves the dependencies, compiles any carried C into the cache, and prints
the compiler flags that implies:

```console
$ spin flags
--require-gate -I /path/pkgs/curses -I /path/backend --link ~/.cache/spin/native/curses-0.1.0-cc/sp_curses.o
```

Every path is absolute, so the caller's working directory can be anywhere:

```make
SPINFLAGS := $(shell cd spin/backend && spin flags)
SPINDEPS  := $(shell cd spin/backend && spin flags --deps)

ruby_app.exe: ruby_app.rb $(RUBY_SRCS) $(SPINDEPS)
	spinel $(SPINFLAGS) -I . $< -o $@
```

`spin flags --deps` prints the toolchain files rather than the flags: the
compiler and its runtime archives. **List them as prerequisites.** A rule whose
prerequisites are only the `.rb` sources answers "up to date" after the
compiler or its runtime changed, and what you run next is a binary the old one
built. That has already cost someone an A/B measurement, where both arms turned
out to be the same compiler and the only tell was that a flag armed in the run
produced no output (#4386). The runtime archives are in the list because a
change under `lib/` rebuilds them and leaves the `spinel` binary untouched --
which is exactly the shape that got missed.

What it prints is what `spin build` compiles with, minus the entry file and
`-o`; the two come from one place, so they cannot drift.

Nothing here is required to consume a package by hand. `require "curses"`
resolves against any `-I` root as `<root>/curses.rb` or
`<root>/curses/curses.rb`, so `spinel -I spin/packages` finds a package sitting
at `spin/packages/curses/`, and `--link` takes its compiled object. `spin
flags` is the part that works out which roots and which objects.

## Shipping a build that does not need spinel

`spin pack` writes a directory that builds the program from C alone: a C
compiler and `make`, no spinel and no spin.

```
$ spin pack
pack demo -> /path/to/demo/build/pack/demo
  cd /path/to/demo/build/pack/demo && make -j
```

It contains the generated C, the runtime sources, the sources of every native
package the build links -- bundled or a dependency, whose objects live in the
shared cache rather than beside their source -- and a Makefile. `--out DIR`
puts it somewhere else. One executable at a time; name it when the project has
several.

**The Makefile is derived, not written.** `spinel --print-build` reports the
ingredients the program requires -- its defines, include paths, libraries and
link inputs, one per line -- and the Makefile is those with their paths
rewritten to the pack's own. A second copy of the build knowledge would drift
from the first, and a drifted copy breaks only on the machine the pack was sent
to, which is the least diagnosable place for it. The same defines reach the
runtime sources for the same reason: a threaded program needs `-DSP_THREADS`
when compiling the runtime as much as when compiling its own translation unit,
and the generated code writes its own `extern`s, so a mismatch there links and
then misbehaves rather than failing.

**What it does not report is a compiler.** Which `cc` compiles the C is the
recipient's decision and not spinel's: a pack cross-compiled for another target
brings its own toolchain, and the ingredients are exactly what has to survive
that. So `CC` and `CFLAGS` in the generated Makefile are defaults rather than
decisions -- `make CC=arm-none-eabi-gcc` builds the same program -- and the
packer's own optimisation level, warning policy, diagnostic formatting and
linker section-GC flags stay behind, where an unfamiliar compiler cannot
reject them.

**The compiler does have to be GCC-compatible.** The runtime headers use
`__attribute__((cleanup))` (every GC root is one), `__COUNTER__`, statement
expressions and `__typeof__`, and the generated C uses statement expressions
throughout. gcc and clang qualify, including cross builds of them; a strict
ISO C compiler does not.

**The runtime travels as source, not as `libspinel_rt.a`.** That is not
thoroughness. The generated C includes the runtime headers and around 270 of
the runtime's functions are `static inline` in them, so the recipient compiles
some 17,000 lines of runtime whichever way it is shipped; the archive would
save the `.c` files and cost the thing the pack is for. An archive is built for
one platform and one set of defines, and a mismatched one is the failure
described above.

**What a pack cannot carry.** A package that binds a system library -- openssl,
sqlite -- names it with `-l` in the Makefile and expects it on the recipient's
machine. Those packs need a C compiler, `make`, and that library. One library
is the recipient's platform's to name rather than the packer's: `String#crypt`
is libc `crypt(3)`, a separate `-lcrypt` on glibc and part of libSystem on
Darwin, and the Makefile decides that from `uname` where it runs, so a pack
made on a Mac links on Linux and the other way round.

**Threads need pthread on the target, and only there.** A program that uses
`Thread` (or `Mutex`, `Queue`, ...) compiles its runtime with `-DSP_THREADS`
and links `-lpthread`; one that does not needs no pthread at all, so it
cross-builds for a target without threads as it is. The threaded pack's
Makefile asks `$(CC)` whether its target has pthread before compiling anything
(a compile-and-link probe, run once), and stops with `<name> uses Thread ...
and <cc> has no pthread: it cannot be built for this target` when it does not,
instead of failing on `<pthread.h>` somewhere inside the runtime. The packer's
own host has no say in it: it may well have pthread when the device does not.

## Rebuilds

`spin build`/`run`/`test` skip recompilation when nothing changed (input
mtimes across the project, its dependencies, and the compiler binary).
There is no file-granular incremental mode -- whole-program type
specialization spans every source -- but compiles are fast and package C
objects are reused from the cache. `spin clean` removes `build/`.

## Command summary

| command | what it does |
|---|---|
| `spin new <name> [--lib]` / `spin init` | scaffold / adopt a directory |
| `spin build [target..]` | compile `bin/` executables into `build/bin/` |
| `spin run [target] [-- args]` | build, then run one executable |
| `spin test [file..] [--regen]` | run `test/*.rb` against snapshots |
| `spin add` / `remove` | edit `[dependencies]` and relock |
| `spin lock` / `fetch` / `vendor` | pin / warm the cache / copy into `vendor/` |
| `spin flags` | print the compiler flags this project implies, for a build driven from outside spin |
| `spin pack` | write a directory that builds the program from C alone: a C compiler and make, no spinel |
| `spin list` / `tree` / `search` (`--json`) | inspect the resolved set / the index |
| `spin publish [--direct]` | validate + test, then submit this release to the index |
| `spin install [name..]` | build and copy `bin/` executables to `~/.local/bin` (`--prefix`, `--uninstall`) |
| `spin clean` | remove `build/` |

Environment: `SPIN_INDEX` (index URL), `SPIN_OFFLINE=1` (cache/vendor
only), `CC` (toolchain for package C), `SPIN_NATIVE_CACHE` (where compiled
package objects go, default `$XDG_CACHE_HOME/spin/native`),
`SPIN_NO_NATIVE_CACHE=1` (recompile package C every time),
`SPINEL_HDR_DIR` (where the runtime headers are, when spin cannot work it out
from the compiler's own path).

### When the cache is in the way

Package `.c` files compile into a shared cache keyed by (package, version,
toolchain), so the same package is not rebuilt for every consumer. That is
worth having across projects and unhelpful while debugging one: a run behaves
differently depending on whether an object happens to be there already, which
is exactly what you do not want when you are trying to find out why a build
differs. `SPIN_NO_NATIVE_CACHE=1` makes every run start from the same state,
and `SPIN_NATIVE_CACHE=<dir>` puts the objects somewhere you can delete.

Note what it does and does not save. The objects are the cheap half: hand-
written C compiles in milliseconds, while whole-program type inference over
the Ruby is where the time goes. The cache exists so a package is not
recompiled once per consuming project, not to make a single build fast.

## Extensions: a Ruby kernel compiled into a CRuby native extension

`spin ext` turns a Ruby method into a C extension for stock CRuby: write the
hot function in Spinel's Ruby subset, keep your application on CRuby.

```
spin ext new fast_math     # scaffold an extension gem
cd fast_math               # edit lib/fast_math/kernel.rb
spin ext build             # emit the C, the shim and the header into ext/
spin ext test              # every case through BOTH paths, answers must match
gem build fast_math.gemspec
```

The kernel is **plain Ruby**: it runs under CRuby unchanged, which makes it
its own fallback (the generated loader `require`s the extension and falls
back to the source) and its own test oracle (`spin ext test` runs each case
through the pure kernel and the compiled one and diffs). The `if __FILE__ ==
$0` block at the bottom is the manual test driver *and* how the exported
methods get their types -- it never runs at extension load. Compiled as
a plain binary (`spinel kernel.rb`) the guard is true, as under `ruby
kernel.rb`, so the same file is also a standalone program.

`spin.toml` names what crosses:

```toml
[ext]
module = "FastMath"
entries = ["FastMath.mandelbrot", "FastMath.render"]
```

Exported methods take and return `Integer`, `Float`, `bool`, `String`, and
typed arrays of these; values cross **by copy**, so mutating a parameter is
refused at compile time (return the result instead). A kernel `raise`
crosses as the same exception class and message. Kernels run **without the
GVL** -- other Ruby threads keep running -- one call at a time.

The built gem ships the generated C and vendors the runtime sources:
installing it needs only a C compiler, never Spinel. `rake-compiler` and
plain `gem install` work as for any hand-written extension.

Layer underneath (any host, not just CRuby): `spinel kernel.rb -c
--ext-init NAME --ext-entry Mod.m,...` emits a library with a host-callable
init function and an `.h` contract that states each entry's C signature;
`spinel --help` lists the flags. `spin ext build` drives exactly this, so
the emitted `ext/<name>/<name>.h` of any scaffolded gem is a worked example
of the contract.
