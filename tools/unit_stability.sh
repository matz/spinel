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
  # no difference at all means the edit never reached the C
  if [ "$n" -eq 0 ] || [ "$n" -gt "$limit" ]; then verdict=FAIL; fail=1; fi
  echo "unit-stability: edit $v ($what): $n of $total lines differ (limit $limit) $verdict"
}
# a and b: the one method's body, in two forms (declaration order can put a
# line in twice: a prototype and a definition)
check a 4 "literal in a body"
check b 4 "constant a method returns"
# c: the method and its callers' arms, bounded well under one unit's worth;
# a leaked counter renumbered thousands of lines here before (2,808 of the
# 8,426 that differed were temps renumbered after the edit)
check c 60 "return type Integer -> Float"
# A definition added before another must leave the other's names alone: a
# node is named after its definition's key, and a key two names spell alike
# goes to the first, the other taking _d1. Each pair is two names a key
# could confuse; the second method's block parameter (y) is named after
# its key, so its names must be the same with the first method and without.
keep() {
  first=$1; second=$2; what=$3
  printf 'class A\n  def %s(x) = [x].map { |y| y }.map { |y| y }\nend\np A.new.%s(1)\n' \
    "$second" "$second" > "$T/one.rb"
  printf 'class A\n  def %s(x) = [x].map { |u| u }.map { |u| u }\n  def %s(x) = [x].map { |y| y }.map { |y| y }\nend\np A.new.%s(1), A.new.%s(1)\n' \
    "$first" "$second" "$first" "$second" > "$T/both.rb"
  for v in one both; do
    "$SP" "$T/$v.rb" -c --no-line-map -o "$T/$v.c" > "$T/$v.log" 2>&1 || { echo "unit-stability: $what: $v.rb does not compile" >&2; fail=1; return; }
    grep -oE 'lv_B[0-9]+_[A-Za-z0-9_]+_y\b' "$T/$v.c" | sort -u > "$T/$v.names"
  done
  if [ -s "$T/one.names" ] && cmp -s "$T/one.names" "$T/both.names"; then verdict=ok; else verdict=FAIL; fail=1; fi
  echo "unit-stability: a method added before $what keeps its names $verdict"
}
LONG=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
keep "${LONG}1" "${LONG}2" "one whose name differs only past 60 characters"
keep "éxc3xa9" "xc3xa9é" "one whose escaped name spells like it"
if [ $fail = 0 ]; then echo "unit-stability: all edits stay local"; else echo "unit-stability: FAILED"; fi
exit $fail
