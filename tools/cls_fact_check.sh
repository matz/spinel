#!/bin/sh
# Holds the class facts (#8530) of each program against a CRuby run of it
# (tools/cls_fact_check.rb). Run from the repository root.
#
#   tools/cls_fact_check.sh [-j N] PROGRAM.rb...
#
# A program's .args and .stdin are used as the test harness uses them. A
# program CRuby cannot run to the end is still checked on what it ran.
# Prints the violations, prefixed by the program, and exits 1 if any; so does a
# check that failed, a program whose trace is empty, and a Ruby that is not 4.0.
set -u
: "${SPINEL:=bin/spinel}"
: "${RUBY:=ruby}"
export SPINEL RUBY
# the observations are CRuby 4.0's: another Ruby, or none, is a failure, not a clean run
"$RUBY" -e 'exit(RUBY_VERSION.start_with?("4.0") ? 0 : 1)' >/dev/null 2>&1 || { echo "cls_fact_check: $RUBY is not a CRuby 4.0 (set RUBY)" >&2; exit 2; }
# Not timeout(1) alone: macOS has none, and every trace came out empty, so every program checked clean.
if command -v timeout >/dev/null 2>&1; then limit30="timeout 30"; else limit30="perl -e alarm(30);exec(@ARGV)"; fi
export limit30

if [ "${1:-}" = "--one" ]; then
  t=$2 base=$3/$(printf '%s' "$2" | tr '/' '_')
  SPINEL_CLS_FACTS=1 "$SPINEL" "$t" -c -o "$base.c" 2> "$base.facts" || exit 0
  args=""; [ -f "$t.args" ] && args=$(cat "$t.args")
  stdinf=/dev/null; [ -f "$t.stdin" ] && stdinf=$t.stdin
  : > "$base.trace"
  # shellcheck disable=SC2086
  CLS_FACT_TRACE_OUT=$base.trace $limit30 "$RUBY" -r ./tools/cls_fact_trace.rb "$t" $args \
    < "$stdinf" > /dev/null 2>&1
  [ -s "$base.trace" ] || echo "$t: EMPTY TRACE (CRuby recorded no call: the program did not load, or ran nothing)"
  out=$("$RUBY" tools/cls_fact_check.rb "$base.facts" "$base.trace" 2> "$base.err"); rc=$?
  [ -n "$out" ] && printf '%s\n' "$out" | sed "s|^|$t: |"
  # 0 clean, 1 violations; anything else is the check failing, which is never clean
  [ "$rc" -le 1 ] || echo "$t: CHECK FAILED (exit $rc): $(head -c 200 "$base.err" | tr '\n' ' ')"
  exit 0
fi

jobs=$(nproc 2>/dev/null || echo 4)
if [ "${1:-}" = "-j" ]; then jobs=$2; shift 2; fi
work=$(mktemp -d "${TMPDIR:-/tmp}/clsfact.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT INT TERM
printf '%s\n' "$@" | xargs -P "$jobs" -I{} sh "$0" --one {} "$work" > "$work/out"
sort "$work/out"
[ ! -s "$work/out" ]
