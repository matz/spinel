#!/bin/bash
# run.sh -- classify extracted ruby/spec examples against spinel.
#
# Usage: tools/rubyspec/run.sh EXTRACTED_DIR [RESULTS_TSV]
#
# Per example: compile with spinel, run, and classify:
#   PASS         compiled, ran, MSPEC-DONE fail=0
#   FAIL         compiled, ran, some expectation failed
#   REJECT       spinel refused to compile (diagnostic recorded + clustered)
#   ERROR        compiled but crashed / timed out / no MSPEC-DONE line
#   HARNESS-SKEW CRuby itself does not pass the extracted program -- the
#                extraction changed meaning; excluded from spinel's score.
#
# Output: one TSV row per example + a summary + the reject-reason ranking,
# which is the "what to implement next" list this harness exists to produce.
#
# Environment knobs:
#   RUBYSPEC_ONLY=<file>  only run the examples named in <file> (one basename
#                         per line, no .rb suffix) -- the retention gate's input
#   RUBYSPEC_GATE=1       skip the CRuby oracle (gate runs re-check examples
#                         already known oracle-clean; CRuby need not be present)
#   RUBYSPEC_JOBS=<n>     parallelism (default: nproc-2, min 1)
#   GATE_CACHE=0          compile and run every example: by default an example
#                         whose generated C, source and build inputs are those
#                         of an earlier PASS reuses that PASS (the result
#                         cache, tools/result_cache.sh; the Makefile's comment
#                         above RUN_ONE_TEST has the reasoning)
set -u
DIR="${1:?usage: run.sh EXTRACTED_DIR [out.tsv]}"
OUT="${2:-$DIR/results.tsv}"
SPINEL="${SPINEL:-bin/spinel}"
ONLY="${RUBYSPEC_ONLY:-}"
GATE="${RUBYSPEC_GATE:-}"
JOBS="${RUBYSPEC_JOBS:-}"
if [ -z "$JOBS" ]; then
  JOBS=$(( $(nproc 2>/dev/null || echo 4) - 2 )); [ "$JOBS" -lt 1 ] && JOBS=1
fi
TDIR=$(mktemp -d /tmp/rubyspec-run.XXXXXX)
trap 'rm -rf "$TDIR"' EXIT

# The result cache. An example's key is its generated C and source plus RC_FP:
# what the driver links against (the runtime archives and package objects
# beside the spinel it runs), the headers the C includes, the driver's own
# source (src/main.c assembles the cc line, src/csplit.c splits a large unit),
# the C compiler it calls (`cc`) and RC_HARNESS, bumped when this file's
# classification changes. Only a PASS is stored.
RC="$(cd "$(dirname "$0")/.." && pwd)/result_cache.sh"
RC_HARNESS=1
RC_FP=
if [ "${GATE_CACHE:-1}" != 0 ]; then
  RC_ROOT=$(cd "$(dirname "$SPINEL")/.." && pwd)
  RC_FP=$(cd "$RC_ROOT" && RC_CC=cc "$RC" fp lib/libspinel_rt.a lib/libspinel_rt_mt.a packages/*/*.o \
    src/main.c src/csplit.c -- "rubyspec $RC_HARNESS $GATE")
fi

# classify_one FILE: write the example's TSV row to $TDIR/rows/<bn>.
# Runs in a parallel worker, so it writes its own file (no shared append).
classify_one() {
  local f="$1"
  local bn; bn=$(basename "$f" .rb)
  local row="$TDIR/rows/$bn"
  local bin="$TDIR/bin-$bn"
  if [ -z "$GATE" ]; then
    # CRuby oracle first: a skewed extraction must not count against spinel.
    local cr; cr=$(timeout 10 ruby "$f" 2>/dev/null | tail -1)
    if ! grep -q "fail=0" <<<"$cr"; then
      echo -e "$bn\tHARNESS-SKEW\t${cr:-crash}" > "$row"; return
    fi
  fi
  # spinel runs every time: its C says whether the example changed. A hit
  # skips the C compile and the run; a miss builds as before, through the
  # driver, so a reject or a cc failure reads exactly as it did.
  local ckey=""
  if [ -n "$RC_FP" ] && "$SPINEL" "$f" -c -o "$TDIR/c-$bn.c" >/dev/null 2>&1; then
    ckey=$("$RC" key "$f" "$TDIR/c-$bn.c" "$RC_FP")
    rm -f "$TDIR/c-$bn.c"
    local hit
    if [ -n "$ckey" ] && hit=$("$RC" get "$ckey") && [ "${hit%%$'\t'*}" = PASS ]; then
      printf '%s\t%s\n' "$bn" "$hit" > "$row"; return
    fi
  fi
  local diag; diag=$("$SPINEL" "$f" -o "$bin" 2>&1 >/dev/null)
  if [ ! -x "$bin" ]; then
    local reason; reason=$(grep -oE "unsupported [^:]*|Parse errors|cannot [a-z ]*|error: [^(]*" <<<"$diag" | head -1)
    # a refusal in the compiler's present wording ("Refinements are not
    # supported by AOT compilation: ...", "undefined method 'x' for a Class:
    # ...") matches none of the forms above; take its message up to the
    # explanation. Read as "unknown", 40% of the rejects hid their construct
    # from the ranking and from gen_manifest's by-design ledger.
    [ -z "$reason" ] && reason=$(sed -nE 's/^spinel: [^ ]+:[0-9]+: //p' <<<"$diag" |
      grep -v '^warning:' | head -1 | sed -E 's/: .*//; s/ \(see docs[^)]*\)//')
    # a rejected call names its method: append it so the reject ranking (and
    # the by-design ledger's rules) can see WHICH call, not just "a call"
    case "$reason" in unsupported*call*|unsupported*argument*)
      local mn; mn=$(grep -oE 'CallNode `[A-Za-z_0-9?!=<>]+`' <<<"$diag" | head -1 | sed 's/CallNode //')
      [ -n "$mn" ] && reason="$reason $mn";;
    esac
    printf '%s\tREJECT\t%s\n' "$bn" "${reason:-unknown}" > "$row"; return
  fi
  local rc last
  # run output goes to a file, NOT a shell variable: an example that prints
  # unboundedly (1.upto(Infinity)) would otherwise balloon the worker
  local run_out="$TDIR/out-$bn"
  timeout 10 "$bin" > "$run_out" 2>&1; rc=$?
  last=$(tail -c 4096 "$run_out" | tail -1)
  rm -f "$bin" "$run_out"
  if [ $rc -ne 0 ] || ! grep -q "MSPEC-DONE" <<<"$last"; then
    # the last line is what the program died with (an uncaught exception's
    # message, if it raised one): gen_manifest's ledger reads it, as it reads
    # a reject's reason
    last=${last//$'\t'/ }; last=${last//$'\r'/}
    printf '%s\tERROR\trc=%s %s\n' "$bn" "$rc" "${last:0:200}" > "$row"
  elif grep -q "fail=0" <<<"$last"; then
    echo -e "$bn\tPASS\t$last" > "$row"
    [ -n "$ckey" ] && echo -e "PASS\t$last" | "$RC" put "$ckey"
  else
    echo -e "$bn\tFAIL\t$last" > "$row"
  fi
}
export -f classify_one
export TDIR SPINEL GATE RC RC_FP

mkdir -p "$TDIR/rows"
if [ -n "$ONLY" ]; then
  # one path per listed basename; a listed example missing from the
  # extraction is an immediate error (manifest and extraction diverged)
  : > "$TDIR/files"
  missing=0
  while IFS= read -r bn; do
    [ -z "$bn" ] && continue
    if [ -f "$DIR/$bn.rb" ]; then echo "$DIR/$bn.rb" >> "$TDIR/files"
    else echo "rubyspec: listed example missing from extraction: $bn" >&2; missing=1; fi
  done < "$ONLY"
  [ "$missing" -ne 0 ] && exit 2
else
  ls "$DIR"/*.rb > "$TDIR/files"
fi

xargs -P "$JOBS" -I{} bash -c 'classify_one "$@"' _ {} < "$TDIR/files"

# stitch rows in stable (example-name) order
: > "$OUT"
for row in $(ls "$TDIR/rows" | sort); do cat "$TDIR/rows/$row" >> "$OUT"; done

pass=$(awk -F'\t'   '$2=="PASS"' "$OUT" | wc -l)
fail=$(awk -F'\t'   '$2=="FAIL"' "$OUT" | wc -l)
reject=$(awk -F'\t' '$2=="REJECT"' "$OUT" | wc -l)
error=$(awk -F'\t'  '$2=="ERROR"' "$OUT" | wc -l)
skew=$(awk -F'\t'   '$2=="HARNESS-SKEW"' "$OUT" | wc -l)
total=$((pass+fail+reject+error))
echo "rubyspec: $pass PASS / $fail FAIL / $reject REJECT / $error ERROR  (of $total; +$skew harness-skew excluded)"
echo "--- top reject reasons ---"
awk -F'\t' '$2=="REJECT"{print $3}' "$OUT" | sort | uniq -c | sort -rn | head -12
