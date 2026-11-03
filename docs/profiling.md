# Profiling a Spinel program

Two measurements, both opt-in and both usable on a normal build: where the time
goes, and where the allocations come from. Neither needs a special compiler and
neither costs anything when it is off.

Before measuring anything, `spinel-doctor app.rb` is the static half of the
same question: its `advice` leg lists the source lines that compile to boxed
dispatch inside loops -- the usual suspects -- without running the program (see
[`tools/README.md`](../tools/README.md)). The profiler below then tells you
which of those actually cost time.

## Where the time goes: `--profile`

```
spinel app.rb --profile -o app
perf record -g ./app
perf report
```

`--profile` builds the same `-O2` binary the default build produces, plus the
three things a sampling profiler needs: `-g`, `-fno-omit-frame-pointer`, and an
unstripped symbol table. It also writes `app.symbols.json` beside the binary --
the `--emit-symbol-map` payload, mapping each emitted C symbol back to the Ruby
name it came from (`sp_PPU_render_pixel` → `Optcarrot::PPU#render_pixel`).

Methods compile to `static` C functions, so a stack walk names them only when
the symbol table is present; that is what `--profile` keeps. If `perf` is
unavailable -- `perf_event_paranoid` is locked down on many CI and hardened
hosts -- any sampler that reads frame pointers works the same way.

### Reading the result

`perf report --stdio --no-children` gives self time per function, which is
usually the first question:

```
84.80%  sp_Interp_visit
 8.49%  sp_StrIntHash_get_opt
 1.82%  __memcmp_evex_movbe
```

`perf script` gives whole stacks, and with `-g` the inlined frames come back
too -- a helper the C compiler folded into its caller still appears by name:

```
    sp_str_byte_len+0x68f (inlined)
    sp_String_append_bin+0x68f (inlined)
    sp_gen_text+0x68f
    main+0x68f
```

Fold those into one line per stack (`a;b;c count`) for a flamegraph renderer.
`app.symbols.json` turns the C names in them back into Ruby ones.

Two things to expect. Kernel frames stay unresolved unless
`/proc/sys/kernel/kptr_restrict` allows otherwise, which does not affect
anything above. And a build that discards unwind information reports no
usable stacks at all -- the same limit the allocation sites have below.

## Where the allocations come from: `SPINEL_ALLOC_REPORT`

```
SPINEL_ALLOC_REPORT=1 ./app            # to stderr
SPINEL_ALLOC_REPORT=alloc.folded ./app # to a file
```

At exit the program dumps one line per allocated type, in the folded-stack
format flamegraph tools read, plus `# bytes` companion lines:

```
alloc;String 1100
alloc;Widget 1000
alloc;Hash(String) 1
alloc;(no-scan) 100
# bytes String 24200
```

`(no-scan)` covers objects with no pointers to trace -- an Integer array, a byte
buffer -- which the collector never has to walk.

Counters key on the object's GC scan callback, which is the de-facto type
identity, and are bumped inside the allocator itself. Strings have no scan
callback, so they carry a reserved key of their own and otherwise behave like
any other row -- including on the per-site path below, which matters because
strings are usually the largest share of the bytes. Nothing is sampled, so
two runs of the same program report the same numbers.

### Per-site attribution: `SPINEL_ALLOC_SITES`

```
SPINEL_ALLOC_REPORT=1 SPINEL_ALLOC_SITES=1 ./app
```

Adds the calling frame as an outer folded frame, so a type allocated from three
places appears three times:

```
alloc;Tree#walk;String 2000
alloc;sp_IntArray_new;(no-scan) 210
alloc;_sp_main_body;Array 199
```

The site is captured as a return address on the counted path and symbolised
only at exit (or at the signalled dump), so the extra cost is one stack walk
per allocation and no allocation of its own. Naming the site costs nothing
until the report is written, and nothing at all with `SPINEL_ALLOC_SITES`
unset.

The name is the best one available, in this order:

1. **The Ruby method** (`Tree#walk`, `Foo.build`, `tally`) when the symbol map
   `--profile` writes, `app.symbols.json`, sits beside the binary under that
   name. A compiler clone of the function (`sp_Tree_walk.constprop.0`) maps to
   the same method.
2. **The C function** (`sp_Tree_walk.constprop.0.isra.0`, `_sp_main_body`,
   `sp_str_repeat`) from the executable's own symbol table. Methods compile to
   `static` functions, which only that table lists; on Linux the report reads
   it from `/proc/self/exe`. `--profile` and `-g` builds keep it, and so does
   the default build -- only an explicitly stripped binary loses it.
3. **What the dynamic symbol table knows**, else the raw address
   (`./app(+0x2bc5) [0x5a0a1e7acbc5]`) -- a stripped binary, or a platform
   other than Linux/ELF. `addr2line -f -e ./app 0x2bc5` on an unstripped copy
   of the same build still turns that into a name.

A site names the function the allocation's caller ended up in after C
inlining, which is not always a method: a method the compiler inlined into
the top-level code shows as `_sp_main_body`, and an allocation made inside a
runtime helper (`sp_IntArray_new`, `sp_str_repeat`) shows that helper. Build
with `-O0` (`spinel app.rb --profile -O0`) to keep every method in its own
frame when the attribution matters more than the speed.

Two caveats:

- The walk needs unwind information. A build that discards it (for instance
  linking with `--gc-sections` on a stripped binary) reports sites as absent
  and falls back to the per-type lines.
- One frame is not always the frame you want: an allocation inside an inlined
  helper is attributed to whatever the compiler left as the caller. Read the
  sites as "the code that asked for this", not as an exact source line.

A program with more distinct *(type, site)* pairs than the stats table holds
does not silently fold them together. What did not fit is reported on its own:

```
alloc;(unattributed) 3
# bytes (unattributed) 78
# note the stats table (8192 entries) was full: 3 allocation(s) could not be
# attributed and are NOT counted in the rows above
```

Every row above that line is still exact -- the overflow is kept out of them
rather than added to whichever row the probe happened to land on.

### While it is still running: a signal

`atexit` is a mode a server cannot use: it is stopped by a signal, so the
counters it spent the whole run filling are lost at the moment they are worth
reading. `SIGUSR1` asks for a dump in flight, and the program carries on:

```
SPINEL_ALLOC_REPORT=alloc.folded ./server &
kill -USR1 $!          # writes alloc.folded now
```

Each dump rewrites the whole cumulative table, so a WINDOW is two dumps
subtracted -- which is also how a server's boot is kept out of the profile:

```
kill -USR1 $pid; sleep 1; cp alloc.folded before.folded
#   ... run the work you want to measure ...
kill -USR1 $pid; sleep 1; cp alloc.folded after.folded
awk 'NR==FNR{a[$1]=$2; next} /^alloc;/{print $1, $2-a[$1]}' before.folded after.folded
```

The `# bytes` lines carry their key in the third field rather than the first.

`SPINEL_ALLOC_REPORT_SIGNAL` moves the signal, by number or by name
(`USR1`, `USR2`, `URG`, `IO`, `WINCH`). Three things worth knowing:

- The handler is installed only while the report is on, but while it is on it
  replaces whatever the program had for that signal. A `Signal.trap("USR1")`
  in the program runs later and replaces it back, at which point the dump
  stops arriving -- move the signal rather than fight over one.
- A threaded program parks a thread on the signal, so the dump arrives whether
  or not anything is allocating. A single-threaded one has no such thread: the
  handler sets a flag and the next allocation writes the report, so an idle
  single-threaded program dumps when it next allocates.
- A process that forked after startup has the handler in the child but not the
  thread; the child falls back to the flag. Signal the parent.

## Which one to reach for

Start with `--profile` and a sampler: it tells you which method to look at.
Reach for `SPINEL_ALLOC_REPORT` when the profile points at the collector
(`sp_gc_collect`, `sp_gc_mark_all`) or at `malloc` -- then the question is not
which code is slow but which code allocates, and the counters answer that
exactly rather than statistically.

## Compiler phase timing

`--timing` (or `SP_TIMING=1` in the environment) makes the compiler report how
long its own phases took. It is about the compiler, not the compiled program
(`--profile` above is the program's). The compiled output is the same with
and without it; with it off nothing is measured.

Each phase is one line on stderr, `key=value` fields after the name:

```
spinel-timing: phase=frontend ms=4.4
spinel-timing: phase=analysis_fixpoint ms=3.2 rounds=4
```

`ms` is wall time on a monotonic clock. Phases, in the order they finish:

| phase | covers |
|---|---|
| `frontend` | parse, require resolution, loading the AST |
| `an_desugar_register`, `an_class_structure`, `an_block_inline`, `an_pre_fixpoint` | the analysis phases ahead of the fixpoint |
| `analysis_fixpoint` | the inference fixpoint (`rounds=N`, plus `capped=1` if it hit the cap) |
| `analysis_renarrow` | the loop that re-narrows poly slots once the fixpoint has settled (`rounds=N`); inside `analysis_fixpoint` |
| `an_post_fixpoint`, `an_procs`, `an_method_backstops`, `an_late_widen`, `an_nullable_returns`, `an_proc_returns`, `an_storage`, `an_value_types`, `an_reconcile_check` | the analysis phases after it |
| `analysis` | all of `analyze_program`; includes the phases above |
| `cg_prepare` | after the analysis, ahead of the C: name clashes, the program's feature scan, the refusals made before any emission |
| `cg_preamble` | the runtime include and the tables and dispatchers emitted ahead of the program's code |
| `cg_classes` | the constructors and the Marshal dispatchers |
| `cg_methods` | the methods, procs and blocks of the program (`methods=N`, the reachable ones) |
| `cg_dispatch` | the dispatchers that call the program's methods (`<=>`, `==`, `hash`, ...) and the `END` blocks |
| `cg_main` | the top-level statements |
| `cg_assemble` | the regexps, the poly dispatch helpers the C uses, and joining the parts |
| `cg_write_barriers` | the GC write barriers, inserted over the whole program |
| `cg_finish` | the rest: frozen literals, line map, the refusal count |
| `codegen_program` | analysis and C generation; includes `analysis` and the `cg_*` phases |
| `write_c` | writing the C file |
| `cc_preprocess`, `cc_split` | a split build (`--jobs=N`, or a large unit) or an `--incremental` build: the preprocess and the split into parts or units |
| `cache_lookup` | an `--incremental` build: the cache keys and the lookup of every unit's object |
| `cc_compile` | a split build: the wall span of the parallel object compiles (`jobs=N parts=M`; `--split-parts=M` cuts more parts than the N compiles at once and starts the biggest first), not the sum of the workers; an `--incremental` build adds `units=N miss=M`, and compiles only the misses |
| `cc_link` | a split build: the link |
| `cc_total` | the whole native step, single unit or split |

A phase nested in another is counted in both, so the lines do not add up to the
total.

Each pass of the inference fixpoint also gets a line, its wall time summed over
the rounds, longest first, right after `analysis_fixpoint`:

```
spinel-timing: pass=infer_write_types ms=47.2 calls=15 changed=12
```

The ten methods whose emission took longest follow `cg_finish`, slowest
first, one line each, named as Ruby names them (`Class#name`, `Class.name`,
a top-level method by its name):

```
spinel-timing: cg_method=Parser#parse_expr ms=812.4
```

`calls` is how often it ran, `changed` how many of those calls reported a
change to the fixpoint (a pass the fixpoint calls only for its effect counts
none). The name is the C function's. The passes are a part of
`analysis_fixpoint` (and of `analysis_renarrow` for the ones its loop runs);
the remainder is the fixpoint's own bookkeeping. With the flag off a pass call
costs a flag test. A phase that did not run (a `-c` build has no `cc_*`) or failed prints
nothing, so a failed build is not a sample. Record the compiler, the input,
the flags, the host and the cache state beside the lines; they are not in them.

### Incremental builds

`--incremental` cuts the generated C into units of about `--unit-bytes=N`
(default `256K`) of preprocessed text, takes the object of every unit whose
text is unchanged from a cache, compiles the misses in parallel (`--jobs=N`,
default the cores up to 16) and links. `--cache-stats` prints
`units=N hit=H miss=M`, the hits per kind and one line per miss. The cache is
`SPINEL_CACHE_DIR`, else `$XDG_CACHE_HOME/spinel`, else
`$HOME/.cache/spinel`; `SPINEL_NO_CACHE=1` compiles everything without
touching it, `SPINEL_KEEP_UNITS=1` keeps the units. A body edit that keeps
its types misses the one unit that holds its class; a changed type misses
the callers' units too; an added instance variable or class misses every
unit of the program but none of the runtime's, which are shared by every
program built with the same flags. The frontend runs in full every time, so
a warm build costs the frontend, a few unit compiles and the link. A
`--target=wasm32-wasi` build splits and caches the same way, with the
wasi-sdk's clang compiling the units and wasm-ld linking them. What it
gives up against a release build is in [limitations.md](limitations.md).

`make incremental-bench` measures it on the scale program. On a 4-core VM,
`gen.sh 100` (`INCREMENTAL_BENCH_K=100`):

| build | wall (s), 256K | units / miss, 256K | wall (s), 512K | units / miss, 512K |
|---|---|---|---|---|
| plain (default) | 40.3 | - | | |
| --jobs=4 | 40.4 | - | | |
| --incremental, cold | 34.9 | 24 / 24 | 33.2 | 12 / 12 |
| warm, a literal in a body | 15.9 | 24 / 1 | 20.4 | 12 / 1 |
| warm, a constant a method returns | 16.8 | 24 / 1 | 20.7 | 12 / 1 |
| warm, a return type Integer -> Float | 17.3 | 24 / 2 | 21.8 | 12 / 2 |
| warm, a method added (and called) | 17.3 | 24 / 2 | 20.8 | 12 / 1 |
| warm, a literal added | 16.6 | 24 / 2 | 21.3 | 12 / 2 |
| warm, an instance variable added | 26.6 | 24 / 20 | 28.9 | 12 / 10 |
| no-op rebuild | 15.9 | 24 / 0 | 14.7 | 12 / 0 |

The C of `gen.sh 100` is over 4 MB, so the plain build is already the
`--jobs` split and the first two rows coincide. About 14 s of every warm
build is the frontend.

`gen.sh 400` (26,000 lines of Ruby, 9.3 MB of C) on the same machine, with
nothing else running:

| build | wall (s), 256K | units / miss, 256K | wall (s), 512K | units / miss, 512K |
|---|---|---|---|---|
| plain (default) | 208.5 | - | | |
| --jobs=4 | 202.1 | - | | |
| --incremental, cold | 176.9 | 79 / 79 | 176.9 | 41 / 41 |
| warm, a literal in a body | 120.3 | 79 / 1 | 127.3 | 41 / 1 |
| warm, a constant a method returns | 118.2 | 79 / 1 | 130.0 | 41 / 1 |
| warm, a return type Integer -> Float | 131.0 | 79 / 2 | 128.9 | 41 / 2 |
| warm, a method added (and called) | 127.7 | 79 / 2 | 128.4 | 41 / 2 |
| warm, a literal added | 127.1 | 79 / 2 | 127.9 | 41 / 2 |
| warm, an instance variable added | 188.6 | 79 / 75 | 172.1 | 41 / 39 |
| no-op rebuild | 127.7 | 79 / 0 | 118.6 | 41 / 0 |

Here the frontend is about 115 s of every build (this VM runs the s400
frontend three times slower than the machine the design was measured on),
so a warm build after a one- or two-unit edit is 118 to 131 s against 208 s
for the plain build, and the two unit sizes are within the run-to-run noise
of each other except on the instance-variable edit, which recompiles every
program and tagged unit and favours the larger units.

## Source positions of a generated .rb

A Ruby file that a tool generated (a template compiler, a transpiler) can say
where its lines came from. A whole line

```ruby
#<SPINEL_SOURCE>greeting.html.erb:12
```

makes the lines after it report as `greeting.html.erb`, line 12, in `#line`
directives, `-g` / `--debug` stepping, `perf` / `addr2line`, `--warn-widen` and
`--check-stores`. The position is held (every following line reports line 12,
not 13, 14, ...) until the next marker or the end of the file it is in. The
text after `>` splits at its last colon, so the path may hold one; a line with
no `:<positive number>` is an ordinary comment. CRuby treats the marker as a
comment too.

Only the reported position moves. `__FILE__`, `__dir__`, `__LINE__` and
`require_relative` still answer from the `.rb` the code is in, and a syntax
error is reported at the generated file. The file's `# spinel: defer-refusals`
comment (docs/limitations.md) also still applies: the lines under a marker
defer their refusals as the `.rb` they are in does, whatever file the marker
names.
