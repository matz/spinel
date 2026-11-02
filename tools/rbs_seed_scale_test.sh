#!/bin/sh
# Applying --rbs signatures finds each method and each earlier declaration by
# name, so the work it adds to a compile grows with the number of signatures.
# A scan of every scope per signature made it quadratic: 4,000 signatures of
# one class cost about 20 times what 1,000 did. The compile's own phases are
# not all linear, so the time of the phase the signatures run in is taken with
# the signatures and without them, and their difference is compared at 1,000
# and 4,000 methods (linear: 4x; limit 8x). The 1,000-method difference counts
# as at least 20 ms, so one that is only timer noise cannot decide the ratio,
# and each difference is the least of five, since the phase is wall time and
# a scheduler pause under a loaded gate would otherwise fail the check (the
# quadratic lookups added 170 ms at 4,000 methods, far above the floor).
# Run from the Spinel root.
set -eu
SPINEL=${SPINEL:-bin/spinel}
mkdir -p build
tmp=$(mktemp -d build/rbs-seed-scale.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM

gen() {
  awk -v n="$1" -v rb="$tmp/s$1.rb" -v rbs="$tmp/s$1.rbs" 'BEGIN {
    print "class RbsScale" > rb; print "class RbsScale" > rbs
    for (i = 0; i < n; i++) {
      printf "def method_%d(value); value + 1; end\n", i > rb
      printf "  def method_%d: (Integer) -> Integer\n", i > rbs
    }
    print "end" > rb; print "end" > rbs
    print "obj = RbsScale.new" > rb
    for (i = 0; i < n; i++) printf "obj.method_%d(1)\n", i > rb
  }'
}

# milliseconds spent before the fixpoint
ms() {
  out=$(SP_TIMING=1 "$SPINEL" "$@" -c -o "$tmp/o.c" 2>&1) || return 1
  m=$(printf '%s\n' "$out" | sed -n 's/^spinel-timing: phase=an_pre_fixpoint ms=\([0-9.]*\).*/\1/p')
  [ -n "$m" ] || return 1
  echo "$m"
}

# what the signatures add: the least of five differences, each taken from a
# compile with them and one without, back to back
added() {
  best=
  for _ in 1 2 3 4 5; do
    p=$(ms "$tmp/s$1.rb") || return 1
    r=$(ms --rbs "$tmp/s$1.rbs" "$tmp/s$1.rb") || return 1
    best=$(awk -v b="$best" -v r="$r" -v p="$p" 'BEGIN { d = r - p; print ((b == "" || d < b + 0) ? d : b) }')
  done
  echo "$best"
}

gen 1000
gen 4000
fail() { echo "rbs-seed-scale-test: FAIL ($1)"; exit 1; }
d1=$(added 1000) || fail "no compile or pre-fixpoint timing row for 1000 methods"
d4=$(added 4000) || fail "no compile or pre-fixpoint timing row for 4000 methods"
awk -v d1="$d1" -v d4="$d4" 'BEGIN {
  f = d1 < 20 ? 20 : d1
  printf "rbs-seed-scale-test: signatures add %.1f ms at 1000 methods, %.1f ms at 4000 (ratio %.1fx over at least 20 ms, limit 8x)\n", d1, d4, d4 / f
  exit (d4 / f > 8) }' || fail "applying --rbs signatures grew superlinearly: a scan of all scopes or declarations per signature"
echo "rbs-seed-scale-test: pass"
