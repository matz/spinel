#!/usr/bin/env bash
# plan_check.sh -- compile the corpus with --plan-check and count the calls
# codegen emitted through a builtin-op row that inference did not answer
# with (#7100).
#
#   tools/plan_check.sh [-v]
#
# A "conflict" (inference answered the call from a different row) fails:
# the two halves of the compiler decided the call differently. An
# "unrecorded" call (inference answered without a row) is reported as a
# count; -v lists both kinds.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SP=$ROOT/bin/spinel
[ -x "$SP" ] || { echo "plan-check: build bin/spinel first" >&2; exit 2; }
JOBS=${PLAN_CHECK_JOBS:-$(nproc)}
OUT=$(mktemp "${TMPDIR:-/tmp}/spinel-plan-check.XXXXXX")
{ ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  [ -f build/optcarrot-single.rb ] && echo build/optcarrot-single.rb; } |
  xargs -P "$JOBS" -I{} sh -c '
    "$2" -c --no-line-map --plan-check "$1" -o /dev/null 2>&1 | grep "^plan-check:" | sed "s|^|$1: |"
  ' _ {} "$SP" > "$OUT"
nc=$(grep -c ': plan-check: conflict:' "$OUT")
nu=$(grep -c ': plan-check: unrecorded:' "$OUT")
[ "${1-}" = "-v" ] && cat "$OUT"
grep ': plan-check: conflict:' "$OUT" | head -20
rm -f "$OUT"
echo "plan-check: $nc conflicts, $nu unrecorded"
[ "$nc" -eq 0 ]
