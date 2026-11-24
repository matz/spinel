#!/bin/sh
# An empty shared ancestry still needs a shadowing walk when a method in
# an unrelated module is named autoload. Conditional includes keep both
# branches of each diamond visible to the record.
set -eu
spinel=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-autoload-diamond.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
printf 'module Root; Entry = 7; end\n' > "$tmp/feature.rb"

diamond() {
  n=$1
  printf 'module Left0; end\nmodule Right0; end\n'
  i=1
  while [ "$i" -le "$n" ]; do
    prev=$((i - 1))
    for side in Left Right; do
      printf 'module %s%s\n include Left%s if ARGV.empty?\n include Right%s if ARGV.empty?\nend\n' "$side" "$i" "$prev" "$prev"
    done
    i=$((i + 1))
  done
  printf 'module Root\n extend Left%s\n extend Right%s\n autoload :Entry, File.join(__dir__, "feature")\nend\nputs Root::Entry\n' "$n" "$n"
}

measure() {
  depth=$1
  diamond "$depth" > "$tmp/control.rb"
  "$spinel" --dump-load-record "$tmp/control.rb" > "$tmp/record" 2> "$tmp/work"
  grep -q '^autoload Root::Entry: write#' "$tmp/record"
  if grep -q '^spinel-work-step: elab_autoload_chain ' "$tmp/work"; then
    echo 'scale-test: FAIL (the control has no possible autoload shadow)' >&2
    return 1
  fi
  cp "$tmp/control.rb" "$tmp/candidate.rb"
  printf 'module Unrelated; def autoload(*) = nil; end\n' >> "$tmp/candidate.rb"
  "$spinel" --dump-load-record "$tmp/candidate.rb" > "$tmp/record" 2> "$tmp/work"
  grep -q '^autoload Root::Entry: write#' "$tmp/record"
  awk '$1 == "spinel-work-step:" && $2 == "elab_autoload_chain" { print $3; found = 1 } END { exit !found }' "$tmp/work"
}

a=$(measure 8)
b=$(measure 16)
awk -v a="$a" -v b="$b" 'BEGIN {
  if (a <= 0 || b <= 0 || b > 3 * a) {
    printf "scale-test: FAIL (autoload diamond work at depths 8 and 16: %d and %d units)\n", a, b
    exit 1
  }
  printf "scale-test: autoload diamond work at 2x the depth is %.2fx (%d, %d units; limit 3.00)\n", b / a, a, b
}'

sed 's/^module Left0; end$/module Left0; def autoload(*) = nil; end/' "$tmp/control.rb" > "$tmp/shadowed.rb"
if "$spinel" -c "$tmp/shadowed.rb" -o "$tmp/shadowed.c" > "$tmp/record" 2> "$tmp/work" ||
   ! grep -qF "loads an autoload's file at its constant's first reference" "$tmp/work"; then
  echo 'scale-test: FAIL (autoload shadow on a shared leaf did not refuse)' >&2
  exit 1
fi
