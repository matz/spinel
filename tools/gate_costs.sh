#!/usr/bin/env bash
# gate_costs.sh -- the run-time cost reports of #7501 as a gate leg.
#
#   tools/gate_costs.sh [REF-REV]
#
# Compares this tree's compiler with the one of REF-REV (default: the
# merge-base of HEAD and origin/master) over the corpus (test/, benchmark/,
# packages/*/test and optcarrot), with
#
#   tools/repr_diff.sh   the representation each slot takes (#7501 item 1)
#   tools/c_costs.sh     the costly constructs in the generated C (item 2)
#
# over the programs whose generated C differs (each compiled once with each
# compiler; an unchanged program is not looked at further),
# and prints their summaries: nothing for a program that did not change, a
# few lines for one that did, small enough to paste into a pull request.
# They are reports, not verdicts: a cost a change brings is explained in
# its pull request, or kept out of the programs that do not need the fix
# (#7501's policy), so this leg does not fail on one. It fails only when it
# cannot run. The reference compiler is built once per revision, in a
# worktree under build/costref/<sha>, and kept for the next run.
#
# On the base itself (HEAD is the reference), or where no reference can be
# found (no origin/master), it says so and passes. The reference is built
# with `make all`: a package's native objects decide how its programs compile.
set -u
set -o pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
NEW=$ROOT/bin/spinel
[ -x "$NEW" ] || { echo "gate-costs: build bin/spinel first" >&2; exit 2; }
REV=${1:-${GATE_COST_REF:-}}
if [ -z "$REV" ]; then
  REV=$(git merge-base HEAD origin/master 2>/dev/null) || { echo "gate-costs: skipped (no origin/master to compare with)"; exit 0; }
fi
SHA=$(git rev-parse --verify -q "$REV^{commit}") || { echo "gate-costs: skipped (unknown revision $REV)"; exit 0; }
if [ "$SHA" = "$(git rev-parse HEAD)" ] && git diff --quiet HEAD -- src lib 2>/dev/null; then
  echo "gate-costs: skipped (this tree is the reference ${SHA:0:9})"; exit 0
fi
JOBS=${GATE_COST_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
REFT=$ROOT/build/costref/$SHA
# one builder at a time per revision (flock on fd 8, dropped when we exit)
mkdir -p "$ROOT/build/costref"
exec 8>"$REFT.lock"
perl -MFcntl=:flock -e 'open(my $fh, ">&=", 8) or die; flock($fh, LOCK_EX) or die' || exit 2
if [ ! -x "$REFT/bin/spinel" ] || [ ! -f "$REFT/.done" ]; then
  git worktree remove --force "$REFT" > /dev/null 2>&1; rm -rf "$REFT"
  git worktree add --detach "$REFT" "$SHA" > /dev/null 2>&1 || { echo "gate-costs: cannot check out ${SHA:0:9}" >&2; exit 2; }
  [ -d "$ROOT/vendor" ] && [ ! -d "$REFT/vendor" ] && cp -rL "$ROOT/vendor" "$REFT/vendor"
  ( cd "$REFT" && make -j"$JOBS" -s all > "$REFT.build.log" 2>&1 ) || { echo "gate-costs: reference build failed (see $REFT.build.log)" >&2; exit 2; }
  : > "$REFT/.done"
  # keep the three newest references
  ls -dt "$ROOT"/build/costref/*/ 2>/dev/null | tail -n +4 | while read -r d; do
    git worktree remove --force "${d%/}" > /dev/null 2>&1; rm -rf "${d%/}" "${d%/}.lock" "${d%/}.build.log"
  done
fi
shopt -s nullglob
programs=(benchmark/*.rb packages/*/test/*.rb test/*.rb)
[ ! -f build/optcarrot-single.rb ] || programs+=(build/optcarrot-single.rb)
# Each program to C once with each compiler; the reports look only at the
# programs whose C differs, so a change that moves nothing costs two
# compiles per program and no more.
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-gate-costs.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
{
  i=0
  for p in "${programs[@]}"; do
    i=$((i + 1)); printf '%06d:%s\0' "$i" "$p"
  done
} | xargs -0 -r -P "$JOBS" -n 1 sh -c '
  k=${4%%:*}; p=${4#*:}; pair=$3/$k
  timeout 120 "$1" -S --no-line-map "$p" > "$pair.ref.c" 2>/dev/null || {
    rm -f "$pair.ref.c"
    # The head invocation can still write requested diagnostic files.
    [ -n "${SPINEL_DECISIONS_LOG+x}${SPINEL_EMIT_TRACE+x}" ] || exit 0
  }
  timeout 120 "$2" -S --no-line-map "$p" > "$pair.new.c" 2>/dev/null || rm -f "$pair.new.c"
  if [ -f "$pair.ref.c" ] && [ -f "$pair.new.c" ] && ! cmp -s "$pair.ref.c" "$pair.new.c"; then
    printf "%s\n" "$p" > "$pair.name"
  else rm -f "$pair.ref.c" "$pair.new.c"; fi
' _ "$REFT/bin/spinel" "$NEW" "$T" || { echo 'gate-costs: could not collect generated C' >&2; exit 2; }
pairs=("$T"/*.name)
nchg=${#pairs[@]}
echo "gate-costs: against ${SHA:0:9}, ${#programs[@]} programs, $nchg whose C differs"
report() {
  local status=0
  "$@" > "$T/report" 2>&1 || status=$?
  head -n "${GATE_COST_LINES:-40}" "$T/report" | sed 's/^/  /' || return 2
  return "$status"
}
if [ "$nchg" -gt 0 ]; then
  repr_args=(--pairs-dir "$T")
  if [ -n "${SPINEL_DECISIONS_LOG+x}${SPINEL_EMIT_TRACE+x}" ]; then
    # Keep the original order of invocations that write diagnostic files.
    LC_ALL=C sort "${pairs[@]}" > "$T/repr-order" || exit 2
    repr_args=()
    while IFS= read -r program; do repr_args+=("$program"); done < "$T/repr-order"
  fi
  REPR_DIFF_JOBS=$JOBS report tools/repr_diff.sh "$REFT/bin/spinel" "$NEW" "${repr_args[@]}" || exit 2
  report tools/c_costs.sh --pairs-dir "$T" || exit 2
fi
echo "gate-costs: report only (#7501): a cost above is explained in the pull request"
exit 0
