#!/usr/bin/env bash
# method_table_check.sh -- compile the corpus with --method-table-check and
# count the reads of the static method tables made outside call_target
# (#10866, step 1).
#
#   tools/method_table_check.sh [--list FILE] [-v]
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
#
# The counters are summed in awk (doubles), so none wraps.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
FAIL_FAMILIES="super send lists"
# the lists family also keeps its static rule: the method list builders and arms read no table themselves
"$ROOT/tools/lists_readers_check.sh" || exit 1
FAMILIES="call implied presence super send lists other pass"
for f in $FAIL_FAMILIES; do
  case " $FAMILIES " in *" $f "*) ;; *) echo "method-table-check: FAIL_FAMILIES names $f, not one of: $FAMILIES" >&2; exit 2 ;; esac
done
LIST=""
VERBOSE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --list) LIST=${2-}; shift 2 || exit 2 ;;
    -v) VERBOSE=1; shift ;;
    *) echo "method-table-check: unknown argument $1" >&2; exit 2 ;;
  esac
done
SP=$ROOT/bin/spinel
if [ -n "$LIST" ] && [ ! -r "$LIST" ]; then echo "method-table-check: cannot read $LIST" >&2; exit 2; fi
[ -x "$SP" ] || { echo "method-table-check: build bin/spinel first" >&2; exit 2; }
JOBS=${METHOD_TABLE_CHECK_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
# optcarrot, packed as the gate packs it (make optcarrot); without its
# checkout there is nothing to pack, and a run without it would pass falsely
if [ -z "$LIST" ] && [ ! -f build/optcarrot-single.rb ]; then
  if [ -f build/optcarrot/tools/pack-for-spinel.rb ] && ruby build/optcarrot/tools/pack-for-spinel.rb > build/optcarrot-single.rb.tmp; then
    mv build/optcarrot-single.rb.tmp build/optcarrot-single.rb
  else
    rm -f build/optcarrot-single.rb.tmp
    echo "method-table-check: build/optcarrot-single.rb is missing and build/optcarrot is not checked out: run make optcarrot" >&2
    exit 2
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
    out=$("$2" -c --no-line-map --method-table-check "$1" -o /dev/null 2>&1); rc=$?
    { printf "%s\n" "$out" | grep "^method-table-check:" | P=$1 awk "{ print ENVIRON[\"P\"] \": \" \$0 }"
      # a program that fails under the flag but not without it is a failure
      if [ "$rc" -ne 0 ]; then
        if "$2" -c --no-line-map "$1" -o /dev/null >/dev/null 2>&1; then echo "$1: method-table-check-flag-exit: $rc"
        else echo "$1: method-table-check-exit: $rc"; fi
      fi; } > "$(mktemp "$3/part.XXXXXX")"
  ' _ {} "$SP" "$PARTS"
if [ $? -ne 0 ]; then
  echo "method-table-check: a compile job could not run" >&2
  rm -rf "${PARTS:?}"
  rm -f "$OUT"
  exit 2
fi
find "$PARTS" -type f -exec cat {} + > "$OUT"
rm -rf "${PARTS:?}"
# a read line: <program>: method-table-check: read <family> <pass|-> <file>:<line> <side>:<Class>#<name> <prim> [versioned]
# $4 the family, $5 the pass, $6 the program position
awk -v verbose="$VERBOSE" -v fail="$FAIL_FAMILIES" '
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
    printf "method-table-check: %.0f programs did not compile, %.0f fail only under the flag\n", failed, flagfail
    for (p in flagged) printf "method-table-check: fails only under the flag (exit %s): %s\n", flagged[p], p
    cmd = "sort -k2,2nr | head -n " (verbose ? 1000000 : 20)
    printf "method-table-check: sites by program position (file:line)\n"; fflush()
    for (w in where) printf "%s %.0f\n", w, where[w] | cmd
    close(cmd)
    printf "method-table-check: passes (sites)\n"; fflush()
    for (p in phase) printf "%s %.0f\n", p, phase[p] | cmd
    close(cmd)
    bad = flagfail > 0
    nf = split(fail, fl, " ")
    for (i = 1; i <= nf; i++)
      if (sites[fl[i]] > 0) { printf "method-table-check: family %s still has %.0f sites\n", fl[i], sites[fl[i]]; bad = 1 }
    exit bad
  }' "$OUT"
rc=$?
rm -f "$OUT"
exit $rc
