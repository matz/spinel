#!/bin/sh
# unit_stability.sh -- an edit to one method changes that method's C alone.
#
#   tools/unit_stability.sh [-k K] [SPINEL]
#
# A name the compiler invents must not depend on what stands before it in
# the program, or a diff of the generated C spreads one edit over every
# function after it: a temp numbered across the file, a proc function
# numbered by the procs before it, a literal numbered by its first use, a
# node named by its place in the table. This check generates the scale
# program of test/scale/gen.sh (K units of model / service / presenter),
# compiles it to C with --no-line-map, then compiles three one-line edits
# of its middle unit and counts the lines of C that differ:
#
#   a  a String literal in a method body changes     -> that method only
#   b  a constant a method returns changes (2 -> 3)   -> that method only
#   c  a method's return type changes (2 -> 2.5)      -> that method, the
#      methods its type flows into, the dispatch that calls them
#
# Each edit has a bound on the differing lines; a count over it means a
# name leaked from somewhere else in the program again, and the check
# fails naming the edit. The counts are printed either way.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
K=400
while [ $# -gt 0 ]; do
  case "$1" in
    -k) K=$2; shift 2 ;;
    *) break ;;
  esac
done
SP=${1:-$ROOT/bin/spinel}
[ -x "$SP" ] || { echo "unit-stability: build bin/spinel first" >&2; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-unit-stability.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
sh "$ROOT/test/scale/gen.sh" "$K" > "$T/base.rb"
MID=$((K / 2))
# the three edits, each of one line of class Model<MID>
awk -v m="$MID" '$0 == "class Model" m " < Base" {f=1} f && /def label = /{sub(/-#\{@tags.size\}/, "+#{@tags.size}"); f=0} {print}' "$T/base.rb" > "$T/a.rb"
awk -v m="$MID" '$0 == "class Model" m " < Base" {f=1} f && /def weight = 2/{sub(/= 2/, "= 3"); f=0} {print}' "$T/base.rb" > "$T/b.rb"
awk -v m="$MID" '$0 == "class Model" m " < Base" {f=1} f && /def weight = 2/{sub(/= 2/, "= 2.5"); f=0} {print}' "$T/base.rb" > "$T/c.rb"
for v in a b c; do
  if cmp -s "$T/base.rb" "$T/$v.rb"; then echo "unit-stability: edit $v did not apply (test/scale/gen.sh changed shape?)" >&2; exit 2; fi
done
fail=0
for v in base a b c; do
  if ! "$SP" "$T/$v.rb" -c --no-line-map -o "$T/$v.c" > "$T/$v.log" 2>&1; then
    echo "unit-stability: $v.rb does not compile:" >&2; tail -3 "$T/$v.log" >&2; exit 2
  fi
done
total=$(grep -c '' "$T/base.c")
check() {
  v=$1; limit=$2; what=$3
  n=$(diff "$T/base.c" "$T/$v.c" | grep -c '^[<>]')
  verdict=ok
  if [ "$n" -gt "$limit" ]; then verdict=FAIL; fail=1; fi
  echo "unit-stability: edit $v ($what): $n of $total lines differ (limit $limit) $verdict"
}
# a and b: the one method's body, in two forms (declaration order can put a
# line in twice: a prototype and a definition)
check a 4 "literal in a body"
check b 4 "constant a method returns"
# c: the method and its callers' arms, bounded well under one unit's worth;
# a leaked counter renumbered thousands of lines here before (3,604 of the
# 8,418 that differed were temps renumbered after the edit)
check c 60 "return type Integer -> Float"
if [ $fail = 0 ]; then echo "unit-stability: all three edits stay local"; else echo "unit-stability: FAILED"; fi
exit $fail
