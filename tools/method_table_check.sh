#!/usr/bin/env bash
# method_table_check.sh -- compile the corpus with --method-table-check and
# count the reads of the static method tables made outside call_target
# (#10866, step 1).
#
#   tools/method_table_check.sh [--list FILE] [-v | -q]
#
# The compiler prints, for each site (a node being inferred or emitted, a
# side and a name):
#   method-table-check: read <family> <pass|-> <file>:<line> <side>:<Class>#<name> <prim> [versioned]
# and, for each answer of call_target that is the table's last definition
# because the record cannot tell (ML_TABLE):
#   method-table-check: table <why> <file>:<line> <side>:<Class>#<name>
# The family is the reader's: call, implied, presence, super, send, lists,
# other, or pass for a read made with no node open; the next field is the
# pass's name for a pass, - otherwise. <file>:<line> is the program's site
# (<synthesized> for a node no source line made, - for none), not the
# compiler's.
#
# Each step of #10866's second stage converts one family to ask call_target,
# and then lists the family in FAIL_FAMILIES (names from the list above): the
# run fails when a listed family has any site left. The run also fails when a
# program compiles without the flag and fails or crashes with it.
#
# A read under a super node of a name other than the super's own (or its
# shadow's or clone's) is another family's: implied, or presence for a
# respond_to? that supers.
#
# A read under a send, a Method maker or an &:sym is the send family's where
# it asks of the name the node sends or captures. Its own name (`send`,
# `method`) is the call's, as any call's is, and another name counts as
# under any call. A run-time name's arms are calls of their own, each the
# frame of what is asked of it.
#
# --list FILE: the programs to compile, one per line, in place of the corpus
# (test/*.rb, packages/*/test/*.rb, benchmark/*.rb, build/optcarrot-single.rb).
# -v: every reader file:line and pass, not the top 20.
# -q: only what fails and the last line (make gate's leg).
# The last line is `method-table-check: pass (...)` or `method-table-check:
# FAIL (...)`, with the program count, the programs that did not compile and
# the FAIL_FAMILIES' sites, or why the check could not run.
# METHOD_TABLE_CHECK_JOBS: the compile jobs (default: every core).
# METHOD_TABLE_CHECK_TIMEOUT: the time limit each compile runs under,
# build/spinel-timeout by default (none when it is not built), with
# METHOD_TABLE_CHECK_LIMIT seconds of CPU time (default 300). A compile that
# times out only under the flag fails only under the flag.
#
# The counters are summed in awk (doubles), so none wraps.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FAIL_FAMILIES="super send lists"
# the run's last line when it stops early: why, and its exit status
stop() { echo "method-table-check: FAIL ($2)"; exit "$1"; }
FAMILIES="call implied presence super send lists other pass"
for f in $FAIL_FAMILIES; do
  case " $FAMILIES " in *" $f "*) ;; *) stop 2 "FAIL_FAMILIES names $f, not one of: $FAMILIES" ;; esac
done
LIST=""
VERBOSE=0
QUIET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --list) [ $# -ge 2 ] || stop 2 "--list needs a file"; LIST=$2; shift 2 ;;
    -v) VERBOSE=1; shift ;;
    -q) QUIET=1; shift ;;
    *) stop 2 "unknown argument $1" ;;
  esac
done
# the lists family also keeps its static rule: the method list builders and arms read no table themselves
if [ "$QUIET" = 1 ]; then
  lr=$("$ROOT/tools/lists_readers_check.sh" 2>&1) || { printf "%s\n" "$lr"; stop 1 "tools/lists_readers_check.sh"; }
else
  "$ROOT/tools/lists_readers_check.sh" || stop 1 "tools/lists_readers_check.sh"
fi
SP=$ROOT/bin/spinel
if [ -n "$LIST" ] && [ ! -r "$LIST" ]; then stop 2 "cannot read $LIST"; fi
[ -x "$SP" ] || stop 2 "build bin/spinel first"
JOBS=${METHOD_TABLE_CHECK_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
TO=${METHOD_TABLE_CHECK_TIMEOUT:-$ROOT/build/spinel-timeout}
[ -x "$TO" ] || TO=""
LIMIT=${METHOD_TABLE_CHECK_LIMIT:-300}
# optcarrot, packed as the gate packs it (make optcarrot); without its
# checkout there is nothing to pack, and a run without it would pass falsely
if [ -z "$LIST" ] && [ ! -f build/optcarrot-single.rb ]; then
  if [ -f build/optcarrot/tools/pack-for-spinel.rb ] && ruby build/optcarrot/tools/pack-for-spinel.rb > build/optcarrot-single.rb.tmp; then
    mv build/optcarrot-single.rb.tmp build/optcarrot-single.rb
  else
    rm -f build/optcarrot-single.rb.tmp
    stop 2 "build/optcarrot-single.rb is missing and build/optcarrot is not checked out: run make optcarrot"
  fi
fi
OUT=$(mktemp "${TMPDIR:-/tmp}/spinel-method-table-check.XXXXXX")
PARTS=$(mktemp -d "${TMPDIR:-/tmp}/spinel-method-table-check-parts.XXXXXX")
# one file per program: the parallel jobs' lines must not interleave. The
# paths go NUL-delimited, so a quote or a space in one stays in it.
{ if [ -n "$LIST" ]; then cat "$LIST"
  else
    for p in test/*.rb benchmark/*.rb packages/*/test/*.rb build/optcarrot-single.rb; do
      if [ -f "$p" ]; then echo "$p"; fi
    done
  fi; } |
  while IFS= read -r path || [ -n "$path" ]; do
    if [ -n "$path" ]; then printf '%s\0' "$path"; fi
  done |
  xargs -0 -P "$JOBS" -I{} sh -c '
    T=$4 L=$5
    # each compile under the time limit, when there is one
    lim() { if [ -n "$T" ]; then "$T" "$L" "$@"; else "$@"; fi; }
    out=$(lim "$2" -c --no-line-map --method-table-check "$1" -o /dev/null 2>&1); rc=$?
    { printf "%s\n" "$out" | grep "^method-table-check:" | P=$1 awk "{ print ENVIRON[\"P\"] \": \" \$0 }"
      # a program that fails under the flag but not without it is a failure
      if [ "$rc" -ne 0 ]; then
        if lim "$2" -c --no-line-map "$1" -o /dev/null >/dev/null 2>&1; then echo "$1: method-table-check-flag-exit: $rc"
        else echo "$1: method-table-check-exit: $rc"; fi
      fi; } > "$(mktemp "$3/part.XXXXXX")"
  ' _ {} "$SP" "$PARTS" "$TO" "$LIMIT"
if [ $? -ne 0 ]; then
  rm -rf "${PARTS:?}"
  rm -f "$OUT"
  stop 2 "a compile job could not run"
fi
find "$PARTS" -type f -exec cat {} + > "$OUT"
PROGS=$(find "$PARTS" -type f | wc -l | tr -d ' ')
rm -rf "${PARTS:?}"
# a read line: <program>: method-table-check: read <family> <pass|-> <file>:<line> <side>:<Class>#<name> <prim> [versioned]
# $4 the family, $5 the pass, $6 the program position
awk -v verbose="$VERBOSE" -v quiet="$QUIET" -v progs="$PROGS" -v fail="$FAIL_FAMILIES" '
  / method-table-check: read / {
    f = $4; sites[f]++; total++
    ver = $NF == "versioned"
    if (ver) vers[f]++
    prim[$(NF - (ver ? 1 : 0))]++
    if (f == "pass") phase[$5]++
    else { sub(/^.*\/bin\/\.\.\//, "", $6); where[$6]++ }
    next
  }
  / method-table-check: table / { why[$4]++; tables++; next }
  / method-table-check-exit: / { failed++; next }
  / method-table-check-flag-exit: / { flagfail++; k = $1; sub(/:$/, "", k); flagged[k] = $NF; next }
  END {
    split("call implied presence super send lists other pass", order, " ")
    if (!quiet) {
      printf "method-table-check: %.0f sites, %.0f answers of the table\n", total, tables
      for (i = 1; i <= 8; i++) {
        f = order[i]
        printf "method-table-check:   %-8s %.0f sites, %.0f versioned\n", f, sites[f], vers[f]
      }
      printf "method-table-check: table answers by why:"
      for (w in why) printf " %s %.0f", w, why[w]
      printf "\n"
      printf "method-table-check: primitives (sites):"
      for (p in prim) printf " %s %.0f", p, prim[p]
      printf "\n"
    }
    if (!quiet) printf "method-table-check: %.0f programs did not compile, %.0f fail only under the flag\n", failed, flagfail
    for (p in flagged) printf "method-table-check: fails only under the flag (exit %s): %s\n", flagged[p], p
    cmd = "sort -k2,2nr | head -n " (verbose ? 1000000 : 20)
    if (!quiet) {
      printf "method-table-check: sites by program position (file:line)\n"; fflush()
      for (w in where) printf "%s %.0f\n", w, where[w] | cmd
      close(cmd)
      printf "method-table-check: passes (sites)\n"; fflush()
      for (p in phase) printf "%s %.0f\n", p, phase[p] | cmd
      close(cmd)
    }
    bad = flagfail > 0
    nf = split(fail, fl, " ")
    counts = ""
    for (i = 1; i <= nf; i++) {
      if (sites[fl[i]] > 0) { printf "method-table-check: family %s still has %.0f sites\n", fl[i], sites[fl[i]]; bad = 1 }
      counts = counts sprintf("%s%s %.0f", i > 1 ? ", " : "", fl[i], sites[fl[i]])
    }
    printf "method-table-check: %s (%.0f programs, %.0f did not compile; %s sites; %.0f fail only under the flag)\n", bad ? "FAIL" : "pass", progs, failed, counts, flagfail
    exit bad
  }' "$OUT"
rc=$?
rm -f "$OUT"
exit $rc
