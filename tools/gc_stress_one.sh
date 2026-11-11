#!/bin/sh
# usage: gc_stress_one.sh SPINEL SPINEL_TIMEOUT TEST
# One program of `make gc-stress-test`: compiled (a promote_* program in the
# mode its answer is defined in), then run under SPINEL_GC_STRESS=2, alone
# and beside the full verifier, each against its .expected. The Makefile
# runs one of these per program in parallel; each has a scratch directory
# of its own, and what it prints is written at once, so parallel runs do
# not interleave. Prints FAIL lines and exits 1 on a failure.
spinel=$1; tmo=$2; src=$3
bn=$(basename "$src" .rb)
fl=
case $bn in promote_*) fl=--int-overflow=promote;; esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-gcstress1.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
msg=
"$spinel" $fl "$src" -o "$tmp/$bn" >"$tmp/cc.out" 2>&1; rc=$?
if [ $rc -ne 0 ]; then
  printf 'gc-stress-test: FAIL (%s: compile, exit %d)\n%s\n' "$bn" "$rc" "$(tail -5 "$tmp/cc.out")"
  exit 1
fi
for v in 0 1; do
  SPINEL_GC_STRESS=2 SPINEL_GC_VERIFY=$v "$tmo" 60 "$tmp/$bn" > "$tmp/out" 2> "$tmp/err"; rc=$?
  if [ $rc -ne 0 ] || ! cmp -s "$tmp/out" "$src.expected"; then
    msg="$msg$(printf 'gc-stress-test: FAIL (%s: SPINEL_GC_STRESS=2 SPINEL_GC_VERIFY=%d, exit %d)\n%s\n%s' \
      "$bn" "$v" "$rc" "$(diff -u "$src.expected" "$tmp/out" | head -10)" "$(head -4 "$tmp/err")")
"
  fi
done
[ -z "$msg" ] && exit 0
printf '%s' "$msg"
exit 1
