# Contributing to Spinel

## Before you open a pull request

**Run `make gate` on your branch, merged with the current `master`, and
paste its summary into the pull request.** The gate builds the compiler and
runs every leg we merge on: the test corpus, the benchmarks, optcarrot, the
ruby/spec retention gate, scale-test, spin-check and the other property
tests. A pull request is merged only after the same gate passes here.

```sh
git fetch origin && git merge origin/master   # or rebase
make gate 2>&1 | tee gate.log
grep -E 'Tests:|scale-test|gate:' gate.log
```

**If `make gate` fails on our side, the pull request goes back to you**
with a comment naming the failing leg. Please fix it and push again; we do
not fix a failing gate for you. When `master` has moved and your branch no
longer merges cleanly, please rebase it.

## What the review checks

- **Same answer as CRuby.** Compare a new test's output with CRuby 4.0
  run with `--enable-frozen-string-literal`: Spinel's string literals are
  always frozen. A path Spinel cannot handle is refused at compile time with
  a message; it must never give a different answer silently.
- **No cost where the change does not apply.** If optcarrot's generated C
  changes, show callgrind numbers; its checksum stays 59662. A rise of more
  than 0.05 in any scale-test ratio is a finding.
- **Tests that run everywhere.**
  - A test whose values pass 2^31 (including through `to_r`, `**` or a
    Bignum) starts with `# spinel: int64`; the 32-bit lane runs every other
    test.
  - Use `Dir.tmpdir` for temporary files, not a fixed `/tmp` path, and no
    OS-specific paths.
  - Give every new test its `.expected` file.
- **Function size.**
  - A function over 1,000 lines does not grow: add a new arm through a
    helper, or in the file for its receiver type.
  - `emit_call_body` only shrinks (#7033).
- **C style.** Helpers are functions, not Ruby-style macros. Generated C
  puts `else` on its own line, not `} else {`. GNU extensions go through
  `sp_compat.h`. `lib/spinel_rt.h` changes are additive only.
- **Mutable Strings (#6179, #6765).** While the share-by-default prototype
  is in progress, a route that silently copies a String a callee appends to
  should be refused at compile time. Please do not add new per-route
  sharing rules.

## Stacked pull requests

If one pull request depends on another, say so in its description, and
keep the shared commits identical (same SHAs) in both, so merging one
brings the other in cleanly.
