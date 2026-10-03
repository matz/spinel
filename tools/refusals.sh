#!/usr/bin/env bash
# refusals.sh -- the refused programs and every message they print.
#
#   tools/refusals.sh [--update] [--corpus]
#
# Compiles every program of test/reject/ and test/collect/ to C, in both
# integer-overflow modes, keeps the `spinel:` lines each one prints, and
# compares them with test/collect/refusals.expected. --corpus adds the cident
# corpus (test/, benchmark/, packages/*/test/), where only the programs that
# refuse are listed; it is kept out of the default run for its time.
# --update rewrites the expected file instead of comparing.
#
# cident proves the set of refused programs does not change; this proves the
# messages do not either. Moving a refusal from codegen into the call plan
# (#7100, CP_REFUSE) must leave this file byte-identical.
#
# Normalized: this tree's root (so lib/ paths are relative) and the node id
# in the internal "unsupported WHAT: node N" dump, which changes with any
# change to the node table. Programs are sorted; each program's lines keep
# the order the compiler printed them in.
#
# Exit status: 0 identical, 1 differs, 2 infrastructure error.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SP=$ROOT/bin/spinel
[ -x "$SP" ] || { echo "refusals: build bin/spinel first" >&2; exit 2; }
EXP=test/collect/refusals.expected
UPDATE=0; CORPUS=0
for a in "$@"; do
  case $a in
    --update) UPDATE=1 ;;
    --corpus) CORPUS=1; EXP=test/collect/refusals-corpus.expected ;;
    *) echo "usage: $0 [--update] [--corpus]" >&2; exit 2 ;;
  esac
done
JOBS=${REFUSALS_JOBS:-$(nproc)}
tmp=$(mktemp -d /tmp/spinel-refusals.XXXXXX) || exit 2
trap 'rm -rf "$tmp"' EXIT

list() {
  if [ $CORPUS = 1 ]; then ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  else ls test/reject/*.rb test/collect/*.rb; fi
}

# one record per program and mode: "== FILE [MODE]" then its spinel: lines
run() {
  list | xargs -P "$JOBS" -I{} sh -c '
    f="$1"; sp="$2"; out="$3"; root="$4"
    key=$(printf "%s" "$f" | tr "/" "_")
    for m in default promote; do
      fl=; [ $m = promote ] && fl=--int-overflow=promote
      "$sp" -c --no-line-map $fl "$f" -o "$out/$key.$m.c" >"$out/$key.$m.err" 2>&1; rc=$?
      rm -f "$out/$key.$m.c"
      { printf "== %s [%s] rc=%s\n" "$f" "$m" "$rc"
        grep "^spinel: " "$out/$key.$m.err" | sed -e "s|$root/||g" \
          -e "s/\(unsupported [^:]*: node \)[0-9][0-9]*/\1N/"
      } > "$out/$key.$m.rec"
      rm -f "$out/$key.$m.err"
      # the corpus lists only the programs that refuse
      if [ -n "$5" ] && [ $rc = 0 ]; then rm -f "$out/$key.$m.rec"; fi
    done
  ' _ {} "$SP" "$tmp" "$ROOT" "$([ $CORPUS = 1 ] && echo 1)"
  for r in $(ls "$tmp" | grep "\.rec$" | LC_ALL=C sort); do cat "$tmp/$r"; done
}

run > "$tmp/actual" || exit 2
if [ $UPDATE = 1 ]; then
  cp "$tmp/actual" "$EXP"; echo "refusals: wrote $EXP ($(grep -c '^== ' "$EXP") records)"; exit 0
fi
[ -f "$EXP" ] || { echo "refusals: no $EXP (run with --update)" >&2; exit 2; }
if cmp -s "$tmp/actual" "$EXP"; then
  echo "refusals: pass ($(grep -c '^== ' "$EXP") records)"; exit 0
fi
echo "refusals: FAIL (the refused programs or their messages changed)"
diff "$EXP" "$tmp/actual" | head -40
exit 1
