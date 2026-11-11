#!/usr/bin/env bash
# line_map_check.sh -- the generated C must not depend on the line map.
#
#   tools/line_map_check.sh [-j JOBS] [--rbs DIR] PROGRAM...
#
# Compiles each program to C twice, with and without --no-line-map, and
# compares the two with the line map taken out: the `#line` lines, and the
# line breaks, since a directive inside an expression (an inlined body's
# statements) starts a line of its own. Anything else that differs is
# codegen that read the directives as code or decided by the length of the
# text it emitted, which they lengthen: a fixed buffer that held a call's
# handle took another path when the text did not fit, and answered
# differently with and without them (#9115). The corpus C diffs compile
# with --no-line-map, so they cannot see it.
#
# Prints each program whose C differs, or that compiles only one way, and
# fails. A program that compiles neither way is left to the suites, but a
# run where none compiled fails: that is a broken compiler, not a clean
# corpus.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SPINEL=${SPINEL:-$ROOT/bin/spinel}
JOBS=2
FLAGS=
while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=$2; shift 2 ;;
    --rbs) FLAGS="$FLAGS --rbs $2"; shift 2 ;;
    *) break ;;
  esac
done
[ -x "$SPINEL" ] || { echo "line-map-check: build bin/spinel first" >&2; exit 2; }
[ $# -gt 0 ] || { echo "usage: $0 [-j JOBS] [--rbs DIR] PROGRAM..." >&2; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-line-map.XXXXXX") || exit 2
trap 'rm -rf "${T:?}"' EXIT
export SPINEL FLAGS T
one() {
  k=$(printf '%s' "$1" | tr '/.' '__')
  "$SPINEL" $FLAGS "$1" -S > "$T/$k.m" 2>/dev/null; rm_=$?
  "$SPINEL" $FLAGS "$1" -S --no-line-map > "$T/$k.n" 2>/dev/null; rn=$?
  if [ $rm_ -ne 0 ] || [ $rn -ne 0 ]; then
    rm -f "$T/$k.m" "$T/$k.n"
    [ $rm_ -ne 0 ] && [ $rn -ne 0 ] && exit 0
    echo "differ $1 (compiles only one way)"; exit 0
  fi
  a=$(grep -v '^#line ' "$T/$k.m" | tr -d '\n' | cksum)
  b=$(grep -v '^#line ' "$T/$k.n" | tr -d '\n' | cksum)
  rm -f "$T/$k.m" "$T/$k.n"
  [ "$a" = "$b" ] && echo "same $1" || echo "differ $1"
}
export -f one
printf '%s\n' "$@" | xargs -P "$JOBS" -I{} bash -c 'one "$1"' _ {} > "$T/out"
ran=$(wc -l < "$T/out" | tr -d ' ')
what="line-map-check${FLAGS:+ ($(echo $FLAGS))}"
[ "$ran" -gt 0 ] || { echo "$what: FAIL (none of $# programs compiled)"; exit 1; }
if grep -q '^differ ' "$T/out"; then
  grep '^differ ' "$T/out" | sed 's/^differ //' | sort
  echo "$what: FAIL ($(grep -c '^differ ' "$T/out") of $ran programs' C depends on the line map)"
  exit 1
fi
echo "$what: ok ($ran of $# programs compiled)"
