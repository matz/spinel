#!/usr/bin/env bash
# plan_check.sh -- compile the corpus with --plan-check and count the calls
# codegen emitted through a builtin-op row that inference did not answer
# with (#7100).
#
#   tools/plan_check.sh [-v]
#
# A "conflict" (inference answered the call from a different row of the
# same receiver kind) fails: the two halves of the compiler decided the
# call differently. An "unrecorded" call (inference answered without a
# row) and a "respecialized" one (the node is emitted per copy with its
# receiver typed per copy) are reported as counts; -v lists all three.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SP=$ROOT/bin/spinel
[ -x "$SP" ] || { echo "plan-check: build bin/spinel first" >&2; exit 2; }
JOBS=${PLAN_CHECK_JOBS:-$(nproc)}
OUT=$(mktemp "${TMPDIR:-/tmp}/spinel-plan-check.XXXXXX")
PARTS=$(mktemp -d "${TMPDIR:-/tmp}/spinel-plan-check-parts.XXXXXX")
# one file per program: the parallel jobs' lines must not interleave. The
# refused programs (test/reject, test/collect) are compiled too, for the
# refusal shadow (refuse-*)
{ ls test/*.rb benchmark/*.rb packages/*/test/*.rb test/reject/*.rb test/collect/*.rb 2>/dev/null
  [ -f build/optcarrot-single.rb ] && echo build/optcarrot-single.rb; } |
  xargs -P "$JOBS" -I{} sh -c '
    "$2" -c --no-line-map --plan-check "$1" -o /dev/null 2>&1 | grep "^plan-check:" | sed "s|^|$1: |" \
      > "$3/$(printf %s "$1" | tr / _)"
  ' _ {} "$SP" "$PARTS"
find "$PARTS" -type f -exec cat {} + > "$OUT"
rm -rf "$PARTS"
nc=$(grep -c ': plan-check: conflict:' "$OUT")
nu=$(grep -c ': plan-check: unrecorded:' "$OUT")
nr=$(grep -c ': plan-check: respecialized:' "$OUT")
uc=$(grep -c ': plan-check: ucall-conflict:' "$OUT")
ur=$(grep -c ': plan-check: ucall-respecialized:' "$OUT")
uv=$(grep -c ': plan-check: ucall-virtual:' "$OUT")
uu=$(grep -c ': plan-check: ucall-unrecorded:' "$OUT")
uo=$(grep -c ': plan-check: ucall-unobserved:' "$OUT")
ue=$(grep -c ': plan-check: ucall-unemitted:' "$OUT")
uf=$(grep -c ': plan-check: ucall-refused:' "$OUT")
ud=$(grep -c ': plan-check: ucall-dynamic:' "$OUT")
# codegen sites that read the plan: a plan naming another method than the
# site's own lookup is a conflict; a plan that cannot serve the site falls back
pc=$(grep -c ': plan-check: cplan-conflict:' "$OUT")
pf=$(grep -c ': plan-check: cplan-fallback:' "$OUT")
# the resolver's per-program counts, summed
rsum=$(grep ': plan-check: ucall-resolver: ' "$OUT" | sed 's/.*ucall-resolver: //' |
  awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) s[i] += $i }
       END { printf "codegen %d agree %d differ %d respecialized %d none; inference %d agree %d differ %d none",
             s[2], s[4], s[6], s[8], s[11], s[13], s[15] }')
[ "${1-}" = "-v" ] && cat "$OUT"
grep ': plan-check: conflict:' "$OUT" | head -20
grep ': plan-check: ucall-conflict:' "$OUT" | head -20
grep ': plan-check: cplan-conflict:' "$OUT" | head -20
echo "plan-check: $nc conflicts, $nu unrecorded, $nr respecialized"
echo "plan-check: resolver: $rsum"
echo "plan-check: plan readers: $pc conflicts, $pf fallbacks"
# per site: the calls each took from the plan, and the ones it fell back on
for site in $(grep ': plan-check: cplan-served: ' "$OUT" | awk '{print $4}' | sort -u); do
  sv=$(grep ": plan-check: cplan-served: $site " "$OUT" | awk '{ s += $5 } END { print s + 0 }')
  fb=$(grep -c ": plan-check: cplan-fallback: $site " "$OUT")
  echo "plan-check:   $site: $sv served, $fb fallbacks"
done
echo "plan-check: user methods: $uc ucall-conflicts, $ur ucall-respecialized, $uv ucall-virtual, $uu ucall-unrecorded, $uo ucall-unobserved, $ue ucall-unemitted, $uf ucall-refused, $ud ucall-dynamic"
# poly dispatch arms: the switch codegen wrote against the resolver's arms
psum=$(grep ': plan-check: poly-arms: ' "$OUT" | sed 's/.*poly-arms: //' |
  awk '{ s += $1; a += $3; x += $5; m += $7; e += $9; k += $11; d += $14 }
       END { printf "%d switches, %d arms, %d poly-conflicts, %d poly-missing, %d poly-extra; trials %d kept, %d dropped",
             s, a, x, m, e, k, d }')
grep ': plan-check: poly-conflict:' "$OUT" | head -20
echo "plan-check: poly arms: $psum"
ppc=$(grep -c ': plan-check: poly-conflict:' "$OUT")
# refusals: the ones codegen reports against the plan's (CP_REFUSE); the
# ones only codegen decides are counted by what is left (codegen_util.c)
rfsum=$(grep ': plan-check: refuse: ' "$OUT" | sed 's/.*refuse: //' | tr -d '(),' |
  awk '{ ok += $1; w += $3; cg += $5; a += $7; s += $9; n += $11; sc += $13; f += $15; u += $17 }
       END { printf "%d refuse-ok, %d refuse-wrong, %d refuse-codegen (%d call, %d shape, %d nomethod, %d string-copy, %d feature), %d refuse-unreached",
             ok, w, cg, a, s, n, sc, f, u }')
grep ': plan-check: refuse-wrong:' "$OUT" | head -20
echo "plan-check: refusals: $rfsum"
rfw=$(grep -c ': plan-check: refuse-wrong:' "$OUT")
rm -f "$OUT"
[ "$nc" -eq 0 ] && [ "$uc" -eq 0 ] && [ "$pc" -eq 0 ] && [ "$ppc" -eq 0 ] && [ "$rfw" -eq 0 ]
