#!/usr/bin/env bash
# repr_check.sh -- compile the corpus with --repr-check and count the boxes
# whose form differs from the one repr_of predicts (#7100 Phase E, R1).
#
#   tools/repr_check.sh [-v]
#
# Each difference is printed by the compiler with what explains it:
#   view           an override of the node's type is active
#   yield-site     a yield boxed by the block of its call site
#   transplant     a module's ivar read boxed by the including class's field
#   ran-first      an argument that already ran: the handle it read then
#   late-slot      a block parameter codegen marked nilable while binding it
#   literal        an empty literal, Hash.new or a splat, boxed by its shape
#   text-vs-node   boxed through emit_boxed_text with a type not the node's
#   conflict       none of the above: repr_box_form and the boxer disagree
# and for each store emit_coerce makes, its form against repr_coerce_form:
#   coerce-view    an override of the node's type is active
#   coerce-conflict  the store and the prediction disagree
# The counts are reported and the commonest conflict shapes listed. The run
# fails on a conflict, and when the C differs with the flag.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SP=$ROOT/bin/spinel
[ -x "$SP" ] || { echo "repr-check: build bin/spinel first" >&2; exit 2; }
JOBS=${REPR_CHECK_JOBS:-$(nproc)}
OUT=$(mktemp "${TMPDIR:-/tmp}/spinel-repr-check.XXXXXX")
PARTS=$(mktemp -d "${TMPDIR:-/tmp}/spinel-repr-check-parts.XXXXXX")
# one file per program: the parallel jobs' lines must not interleave. Each
# program is compiled with and without the flag, and the two C files compared.
{ ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  [ -f build/optcarrot-single.rb ] && echo build/optcarrot-single.rb; } |
  xargs -P "$JOBS" -I{} sh -c '
    k=$(printf %s "$1" | tr / _)
    "$2" -c --no-line-map --repr-check "$1" -o "$3/$k.on.c" 2>"$3/$k.err" >/dev/null
    "$2" -c --no-line-map "$1" -o "$3/$k.off.c" >/dev/null 2>&1
    if [ -f "$3/$k.on.c" ] || [ -f "$3/$k.off.c" ]; then
      cmp -s "$3/$k.on.c" "$3/$k.off.c" || echo "$1" > "$3/$k.cdiff"
    fi
    grep "^repr-check:" "$3/$k.err" | sed "s|^|$1: |" > "$3/$k.log"
    rm -f "$3/$k.on.c" "$3/$k.off.c" "$3/$k.err"
  ' _ {} "$SP" "$PARTS"
find "$PARTS" -name '*.log' -type f -exec cat {} + > "$OUT"
ndiff=$(find "$PARTS" -name '*.cdiff' | wc -l)
find "$PARTS" -name '*.cdiff' -exec cat {} + | head -10
rm -rf "$PARTS"
count() { grep -c ": repr-check: $1:" "$OUT"; }
[ "${1-}" = "-v" ] && cat "$OUT"
echo "repr-check: top conflict shapes (node kind, type, emitted, predicted):"
grep ': repr-check: conflict:' "$OUT" |
  sed -E 's/.*: node [0-9]+ ([A-Za-z]+) ([^:]+): emitted ([A-Z_]+), predicted ([A-Z_]+)$/\1 \2 \3 \4/' |
  sort | uniq -c | sort -rn | head -15
echo "repr-check: $(count conflict) conflicts, $(count view) view, $(count yield-site) yield-site," \
     "$(count transplant) transplant," \
     "$(count ran-first) ran-first, $(count late-slot) late-slot, $(count literal) literal," \
     "$(count text-vs-node) text-vs-node"
echo "repr-check: top coerce conflict shapes (node kind, from->slot, emitted, predicted):"
grep ': repr-check: coerce-conflict:' "$OUT" |
  sed -E 's/.*: node [0-9]+ ([A-Za-z?]+) ([^:]+): emitted ([A-Z_0-9]+), predicted ([A-Z_0-9]+)$/\1 \2 \3 \4/' |
  sort | uniq -c | sort -rn | head -15
echo "repr-check: stores: $(count coerce-conflict) coerce-conflicts, $(count coerce-view) coerce-view"
echo "repr-check: $ndiff programs whose C differs with the flag"
nconf=$(count conflict)
ncc=$(count coerce-conflict)
grep ': repr-check: conflict:' "$OUT" | head -20
grep ': repr-check: coerce-conflict:' "$OUT" | head -20
rm -f "$OUT"
[ "$ndiff" -eq 0 ] && [ "$nconf" -eq 0 ] && [ "$ncc" -eq 0 ]
