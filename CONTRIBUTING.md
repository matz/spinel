# Contributing to Spinel

## Before you open a pull request

**Run `make gate` on your branch, merged with the current `master`, and
paste its summary into the pull request.** The gate builds the compiler and
runs every leg we merge on: the test corpus, the benchmarks, optcarrot,
rubyboy, the ruby/spec retention gate, scale-test, spin-check and the other
property tests. A pull request is merged only after the same gate passes here.

```sh
git fetch origin && git merge origin/master   # or rebase
make gate 2>&1 | tee gate.log
grep -E 'Tests:|scale-test|gate:' gate.log
```

The gate does not compile and run again a program whose C has not changed
since a passing run; `make gate-full` runs everything.

**For the fastest `make gate` runs, gate several branches as one batch.**
Base them on the same `master` commit and gate them one after another in the
same checkout, whose result cache then skips most of the programs a branch
leaves unchanged. Separate checkouts each build their own runtime libraries,
so sharing one cache between them through `RESULT_CACHE_DIR` saves less.

**Install `sccache` or `ccache`.** `common.mk` then wraps `cc` in it, so a
build of the compiler reuses each object an earlier build compiled the same
way, in any checkout.

To keep the machine usable while the gate runs, pin the gate to a chosen set
of N CPUs and set `MAKEFLAGS=-jN` beside `taskset`: the job count comes from
`getconf _NPROCESSORS_ONLN`, which counts every CPU.

```sh
MAKEFLAGS=-j12 taskset -c 0-11 make gate   # N = 12 jobs, on CPUs 0-11 only
```

**If `make gate` fails on our side, we fix it where the fix is mechanical** (a
recorded list such as `test/collect/refusals.expected`, a test whose output
depends on an order the implementation chooses) and say what we changed in the
pull request. A failure that needs a design decision goes back to you with a
comment naming the failing leg.

**If your branch conflicts with `master` or with other open pull requests, we
resolve it** and say how in the pull request. Many pull requests touch the same
functions, so keep a branch small; a smaller one conflicts less and is quicker
to take.

## Issues and pull requests

**A pull request needs no issue.** A bug fix with its reproducer as a test,
and the `spinel diff` report if the answer differs from CRuby's, goes in as
one self-contained pull request. Do not open an issue first and then a pull
request for it a few minutes later: it costs the same review twice.

**An open issue means "I will not work on this", or "let us talk".** If you
mean to fix something, open a draft pull request straight away; that is also
how others see it is taken. If you only want to report it, or will not get to
it, open an issue and someone else may pick it up. Issues are also the place
for discussion: a direction to decide, a proposal, a question.

**Describe the why, not the history.** A pull request that fixes a bug needs
a few lines: what was wrong, and the reproducer. Write more when the change is
a design decision (what you chose and what you rejected); do not retell how
the implementation got there or link every related issue.

## The pre-commit checks

Run `make hooks` once: it points git at the hooks in `tools/hooks`. The
pre-commit hook (`ruby tools/gate.rb check`) refuses a commit that grows
`emit_call_body` or any function over 1,000 lines, adds a test whose
`.expected` differs from CRuby run with `--enable-frozen-string-literal`
(unless the test is marked `# spinel: not-cruby`, below), or writes to a
fixed `/tmp` path. CI runs the same checks on a pull request's commits.
Where `make gate` can't pass natively, `ruby tools/gate.rb linux` runs it in
a Linux container on this branch merged with `master`.

The `.expected` comparison uses `GATE_RUBY`, else `ruby` from `PATH` when it
is Ruby 4.0 or later (`tools/gate-ruby` picks it); without such a Ruby it is
skipped with a warning. `make gate-tool-test` tests `tools/gate.rb` itself.

`make share-verify-test` compiles `test/share/*.rb`, `test/share_strings_*.rb`
and `test/share/verify/**/*.rb` with `--repr-check --plan-check`.
It fails on a new conflict, a failed compilation, or generated C that differs
with the checking flags, or an output mismatch. Each executable program runs
once against its `.expected`, without GC stress. The two examples under
`test/share/verify/conflicts/` remain compile-only; their expected files record
CRuby answers that the compiler does not yet produce. Verifier fixtures have
no `share` marker and do not join `share-strings-test`. A stale entry in `test/share/verify-conflicts.txt`
prints a removal reminder and does not fail the target. The ratchet contains
exact diagnostics, one per line; remove an entry when its disagreement is fixed.
`SHARE_VERIFY_JOBS` controls parallelism (default 2).
Use `ruby tools/share_verify.rb -v` for classified observations and coverage
gaps, or pass Ruby files to check a focused subset. `--extra DIR` includes
another directory's Ruby files. This target is separate from `make gate`.

The String channel shadow records the existing return-tail analysis and
observes publishing reads, fresh-tail clearing, nil guards at pickups, and
the existing polymorphic arm records. Boxed proc/block results use their
own value channel. `channel-unobserved` identifies a publication or target
the shadow cannot verify; it is not a clean bill of health. In particular,
the shadow does not yet prove channel preservation across an ensure body
that publishes another value. The ordinary repr shadow still checks boxes
and coercions; not every handwritten argument or element store uses it.

## What the review checks

- **Same answer as CRuby, or a refusal.** Compare a new test's output with
  CRuby 4.0 run with `--enable-frozen-string-literal`: Spinel's string
  literals are always frozen. A path Spinel cannot handle is refused at
  compile time with a `spinel: FILE:LINE: ...` message naming the construct.
  A program Spinel compiles must behave as CRuby does: it must not give a
  different answer silently, raise an error CRuby would not raise (a
  `NoMethodError` for a method that was never defined, a
  `NotImplementedError` for an unsupported construct), or fail in the C
  compiler. Turning a refusal into a runtime `NotImplementedError` is only
  for `--defer-refusals`, which a user asks for explicitly.
  `docs/limitations.md` describes the refusals; when a change supports or
  refuses a construct, update its entry in the same pull request.
- **No cost where the change does not apply.** If optcarrot's generated C
  changes, show callgrind numbers; its checksum stays 59662. A rise of more
  than 0.05 in any scale-test ratio is a finding.
- **Tests that run everywhere.**
  - A test whose values pass 2^31 (including through `to_r`, `**` or a
    Bignum) starts with `# spinel: int64`; the 32-bit lane runs every other
    test.
  - A test whose `.expected` is Spinel's own answer and legitimately differs
    from CRuby's (a refusal message, a Spinel-only API) carries
    `# spinel: not-cruby` and a reason in its first lines; the pre-commit
    hook then does not compare it with CRuby.
  - Use `Dir.tmpdir` for temporary files, not a fixed `/tmp` path, and no
    OS-specific paths.
  - Give every new test its `.expected` file.
  - A new test registers itself with these header lines; no Makefile edit:
    `# spinel: share` adds it to `share-strings-test`, which runs it again
    under GC stress, `# spinel: gc-minor`
    runs it with the minor mark off, on, and under the generational verifier
    with GC stress, and `# spinel: gc-stress` runs stress level 2 with and
    without the full verifier. Use one line per marker.
    `# spinel: wasm` selects the WASI smoke tests; `# spinel: decisions`
    selects the decision checks. In `test/wasm_abort/`, `# spinel: wasm-abort`
    selects a program built with `--exceptions=abort`,
    `# spinel: wasm-abort-raises: MESSAGE` the message its raise traps with,
    and `# spinel: wasm-abort-refuse: REASON` the words of its refusal.
    In `test/rbs-seed/`, `# spinel: rbs-seed-run`
    selects an output check and `# spinel: rbs-seed-check` selects an existing
    named assertion in the RBS harness. Every `test/rbs-seed/` program with an
    `.expected` also runs with its signatures in `share-strings-test`, plain
    and under GC stress; `# spinel: rbs-seed-no-share` keeps one out (say why
    in its header).
    `# spinel: needs-rbs-parser` keeps a `test/` program whose inline RBS
    annotations decide what it prints out of a build without the rbs C
    parser (`make deps` fetches it), where the annotations are comments.
    `# spinel: linux` keeps a test whose subject only Linux gives to Linux
    hosts. `# spinel: no-cache` makes the gate always run a test, for an input
    the gate's result cache cannot see.
    The other `reject-*` markers select the diagnostic assertion named in
    `reject-test`; `rbs-seed-contradicted-return` selects its grouped refusal
    check, and `infer-ivar-get` selects the boxed instance-variable read check.
    A rejection marker followed by `: text` supplies the literal diagnostic
    text to match. In `test/defer/`, `# spinel: defer-refusals: count:output`
    gives the refusal count and stdout, with output lines separated by spaces.
    In `test/load_graph/`, `# spinel: exit N` gives the exit status a
    program's run ends with (0 without it).
    The ordinary corpus, `test/share/*.rb`, `test/share_strings_*.rb` and
    `test/share/refuse/*.rb` are still discovered by name. Keep `int64` on
    its own first line when needed; platform and overflow exclusions still
    apply.
- **Function size.**
  - A function over 1,000 lines does not grow: add a new arm through a
    helper, or in the file for its receiver type.
  - `emit_call_body` only shrinks (#7033).
- **C style.** Helpers are functions, not Ruby-style macros. Generated C
  puts `else` on its own line, not `} else {`. GNU extensions go through
  `sp_compat.h`. `lib/spinel_rt.h` changes are additive; a change to
  existing runtime code is accepted case by case, when the fix's natural
  home is there and the pull request explains why the additive alternative
  is worse (#8328). An emitter that needs a node to read differently for one
  emission uses a view (docs/internals/architecture.md, "Views"); it never
  writes the node table or a global for it.
- **One place per decision (#7100, #8328).** The analysis and
  representation layer decides once, and the emitters read that decision.
  [docs/internals/architecture.md](docs/internals/architecture.md) lists the
  layers, the question each one answers and its entry points. Three rules:
  put a fact in the layer that owns it; convert old-style code on the lines
  you touch; when the right layer lacks the fact, add it there rather than
  special-casing one call site.
- **Strings (#9258).** A String is one object, as in CRuby: every String
  slot holds its handle, so a change made through one name is seen through
  every other name that holds the same String (`dup`, `clone` and
  `String.new` make a new one). It is the only representation. The share
  analysis that chose between two representations is gone; what is left of
  the String plan and the byte routes (`share_plan_*`, `strbuf_*`,
  `emit_strbuf_*`) is being removed, so a change does not add to them. A
  program that answers differently from CRuby is welcome as a new issue,
  with a reduction.

## Stacked pull requests

If one pull request depends on another, say so in its description, and
keep the shared commits identical (same SHAs) in both, so merging one
brings the other in cleanly.
