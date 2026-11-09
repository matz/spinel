#!/bin/sh
# usage: share_strings_one.sh SPINEL TEST
# One program of `make share-strings-test`: compiled, run, and run again under SPINEL_GC_STRESS=1, each against its .expected
# (and its .err.expected for stderr, when it has one). A test/rbs-seed program
# compiles with its signatures (--rbs test/rbs-seed/sig), and its .expected is
# named without the .rb, as rbs-seed-test has it. The Makefile runs
# one of these per program in parallel; each has a scratch directory of
# its own. Prints a FAIL line and exits 1 on a failure; what it prints is
# collected and written at once, so parallel runs do not interleave.
spinel=$1; t=$2
e="$t.expected"
rbs=
case "$t" in
  test/rbs-seed/*) e="${t%.rb}.expected"; rbs="--rbs test/rbs-seed/sig";;
esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-share1.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
check_output() {
  if [ -f "$t.err.expected" ]; then
    "$tmp/b" >"$tmp/stdout" 2>"$tmp/stderr"
    cmp -s "$tmp/stdout" "$e" && cmp -s "$tmp/stderr" "$t.err.expected"
  else "$tmp/b" 2>&1 | cmp -s - "$e"; fi
}
ok=0
{
  if "$spinel" $rbs "$t" -o "$tmp/b" >"$tmp/out" 2>&1; then
    ! grep -q 'did not converge' "$tmp/out" || { echo "share-strings-test: FAIL $t (the inference fixpoint ran to its round cap)"; ok=1; }
    check_output || { echo "share-strings-test: FAIL $t"; ok=1; }
    SPINEL_GC_STRESS=1 check_output || { echo "share-strings-test: FAIL $t (GC stress)"; ok=1; }
  else
    echo "share-strings-test: FAIL $t (refused)"; cat "$tmp/out"; ok=1
  fi
  echo $ok > "$tmp/rc"
} > "$tmp/msg" 2>&1
cat "$tmp/msg"
exit "$(cat "$tmp/rc")"
