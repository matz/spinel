#!/usr/bin/env bash
# rt_san_check.sh -- run the corpus against the runtime built under
# AddressSanitizer and UndefinedBehaviorSanitizer and list what they report.
#
#   tools/rt_san_check.sh [-v] [file.rb ...]      (make rt-san-check)
#   tools/rt_san_check.sh [-v] --summary DIR      (a run kept with RT_SAN_KEEP=1)
#
# san_check.sh checks the compiler; this checks what the compiled programs
# run on. The runtime archives, the regexp engine and the bundled packages'
# objects are built with the sanitizers into build/rt-san (the Makefile's
# rt-san-check prerequisites). Every program of test/ and packages/*/test/
# that `make test` runs here (or the files named) is compiled to C by
# bin/spinel, the plain compiler, and its C is compiled with the same
# flags, since most of the runtime is spinel_rt.h's inline code in the
# program's own translation unit, then linked against those objects and
# run twice: as it is; and with SPINEL_GC_STRESS=2, which collects at every
# allocation and poisons what dies.
#
# A run is made as `make test` makes it (RUN_ONE_TEST in the Makefile): from
# the repository root, with the program's .args on its command line and its
# .stdin (else /dev/null) on standard input, the descriptor limit raised, the
# threaded archive and packages for a program that uses threads, the
# SPINEL_LINK libraries the C names, and the level its .rb.opt names (else
# -O0, which the harness takes for a C file of 2,000 lines or more). The CPU limit is RT_SAN_TIMEOUT seconds
# (default 30, the harness's 10 times the sanitizers' slowdown); a run past
# it is counted, not a finding.
#
# One line a site: the sanitizer's file:line for undefined behaviour, the
# first frame in the runtime, a package or the program's C for a memory
# error, and the GC stress check's own report of a freed slot the mark
# reached; with the number of programs that reach it and the first of them,
# and which run. -v adds that run's report. The sanitizers write to a log of
# their own (log_path), so the program's stderr is still its own; its
# output is compared to .expected (and .err.expected) only as a sanity
# check, and the runs that differ without a report are counted, as are the
# programs the compiler refuses and those whose C did not build. Leaks are
# not reported: the collector, not free, ends most objects. Programs `make
# test` leaves out here (by mode, host or missing library) are named on the
# skipped line; RT_SAN_SKIP carries them from the Makefile.
#
# Exit status: 0 no report, 1 some program reported, 2 infrastructure error
# (something is not built, there is no program, or xargs did not run them
# all).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

# One program, under xargs: the runs, and what each left in $LOGS
if [ "${1-}" = "--one" ]; then
  key=${2%%:*}
  f=${2#*:}
  d=$RT_SAN_LOGS/$key
  args=""
  [ -f "$f.args" ] && args=$(cat "$f.args")
  stdinf=/dev/null
  [ -f "$f.stdin" ] && stdinf=$f.stdin
  ulimit -n 4096 2>/dev/null || true
  # build C file -> binary, as RUN_ONE_TEST picks the flags
  build() {
    local c=$1 bin=$2 opt="" mtdef="" rtlib=$RT_SAN_LIB objs=$RT_SAN_PKG_OBJS mtld="" xlibs
    xlibs=$(sed -n 's|^/\* SPINEL_LINK: \(.*\) \*/$|\1|p' "$c" | tr '\n' ' ')
    [ -f "$f.opt" ] && opt=$(cat "$f.opt")
    if grep -q SPINEL_USES_THREADS "$c"; then
      mtdef=$RT_SAN_MT_DEF rtlib=$RT_SAN_MT_LIB objs=$RT_SAN_PKG_MT_OBJS mtld=-lpthread
    fi
    # Compiled and linked apart: in one step the driver runs dsymutil, and
    # the symbolizer reads the object's debug information through the
    # binary's map instead, while the object is there. The words of each
    # list are separate arguments, as in the recipe.
    # shellcheck disable=SC2086
    $RT_SAN_CC $RT_SAN_CFLAGS $opt $mtdef -Ilib -c "$c" -o "$bin.o" > "$bin.log" 2>&1 &&
      $RT_SAN_CC $RT_SAN_CFLAGS "$bin.o" $objs $rtlib $RT_SAN_LDFLAGS -lm $mtld $xlibs -o "$bin" >> "$bin.log" 2>&1
  }
  # run binary as run name `r`, SPINEL_GC_STRESS=$3 (empty: unset)
  run() {
    local bin=$1 r=$2 st=$3 rc
    # shellcheck disable=SC2086
    if [ -n "$st" ]; then
      SPINEL_GC_STRESS=$st ASAN_OPTIONS="$RT_SAN_ASAN:log_path=$d.$r.san" UBSAN_OPTIONS="$RT_SAN_UBSAN:log_path=$d.$r.san" \
        "$RT_SAN_TIMEOUT_BIN" "$RT_SAN_TIMEOUT" "$bin" $args < "$stdinf" > "$d.$r.out" 2> "$d.$r.err"
    else
      env -u SPINEL_GC_STRESS ASAN_OPTIONS="$RT_SAN_ASAN:log_path=$d.$r.san" UBSAN_OPTIONS="$RT_SAN_UBSAN:log_path=$d.$r.san" \
        "$RT_SAN_TIMEOUT_BIN" "$RT_SAN_TIMEOUT" "$bin" $args < "$stdinf" > "$d.$r.out" 2> "$d.$r.err"
    fi
    rc=$?
    # a report: an error in the sanitizers' log (one per process, named by
    # its pid; a warning alone is none), or the stress check's text, which
    # goes to stderr before its abort. ASan's own failure to unmap is no
    # report on the program: it ends the run, which is then not checked.
    if ls "$d.$r.san".* > /dev/null 2>&1; then
      cat "$d.$r.san".* > "$d.$r.san"
      if grep -q 'ERROR: AddressSanitizer: \|runtime error:' "$d.$r.san"; then
        mv "$d.$r.san" "$d.$r.report"
      elif grep -q 'ERROR: AddressSanitizer failed' "$d.$r.san"; then
        echo "$r $f: $(grep -m1 'ERROR: AddressSanitizer failed' "$d.$r.san" | sed 's/^==[0-9]*==//')" > "$d.$r.sanfail"
      fi
      rm -f "$d.$r.san"
    fi
    if grep -q 'SPINEL_GC_STRESS: the mark reached a freed slot' "$d.$r.err"; then
      { echo "SPINEL_GC_STRESS: the mark reached a freed slot"; grep -A3 'SPINEL_GC_STRESS: the mark' "$d.$r.err"; } >> "$d.$r.report"
    fi
    rm -f "$d.$r.san".*
    if [ -f "$d.$r.report" ] || [ -f "$d.$r.sanfail" ]; then
      :
    elif [ "$rc" -eq 124 ]; then
      echo "$r $f" > "$d.$r.timeout"
    elif [ -f "$f.expected" ]; then
      LC_ALL=C sed 's/\r$//' "$f.expected" > "$d.exp"
      LC_ALL=C sed 's/\r$//' "$d.$r.out" > "$d.act"
      if [ -f "$f.err.expected" ]; then LC_ALL=C sed 's/\r$//' "$f.err.expected" > "$d.experr"; else : > "$d.experr"; fi
      LC_ALL=C sed 's/\r$//' "$d.$r.err" > "$d.acterr"
      cmp -s "$d.exp" "$d.act" && cmp -s "$d.experr" "$d.acterr" || echo "$r $f exit $rc" > "$d.$r.diff"
      rm -f "$d.exp" "$d.act" "$d.experr" "$d.acterr"
    fi
    rm -f "$d.$r.out" "$d.$r.err"
  }
  if ! "$RT_SAN_SPINEL" -c --no-line-map "$f" -o "$d.d.c" > "$d.d.cerr" 2>&1; then
    echo "d $f: $(tail -n 1 "$d.d.cerr")" > "$d.d.left"
    rm -f "$d.d.cerr"
  else
    rm -f "$d.d.cerr"
    if ! build "$d.d.c" "$d.d.bin"; then
      { echo "d $f"; cat "$d.d.bin.log"; } > "$d.d.nobuild"
    else
      run "$d.d.bin" d ""
      run "$d.d.bin" g2 2
    fi
    rm -f "$d.d.bin.log"
  fi
  rm -f "$d.d.c" "$d.d.bin" "$d.d.bin.o"
  : > "$d.ran"
  exit 0
fi

VERBOSE=0
[ "${1-}" = "-v" ] && { VERBOSE=1; shift; }
# --summary DIR prints the sites of a run kept with RT_SAN_KEEP=1 again
SUMMARY=""
[ "${1-}" = "--summary" ] && { SUMMARY=${2-}; shift 2; [ -f "$SUMMARY/list" ] || { echo "rt-san-check: $SUMMARY holds no kept run" >&2; exit 2; }; }
# what the Makefile passes; the defaults are its own paths
: "${RT_SAN_SPINEL:=$ROOT/bin/spinel}"
: "${RT_SAN_LIB:=build/rt-san/libspinel_rt.a}"
: "${RT_SAN_MT_LIB:=build/rt-san/libspinel_rt_mt.a}"
: "${RT_SAN_TIMEOUT_BIN:=$ROOT/build/spinel-timeout}"
: "${RT_SAN_TIMEOUT:=30}"
: "${RT_SAN_CC:=cc}"
: "${RT_SAN_CFLAGS:=-O0 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fsanitize-recover=address}"
: "${RT_SAN_MT_DEF:=-DSP_THREADS -ftls-model=initial-exec}"
: "${RT_SAN_PKG_OBJS:=}" "${RT_SAN_PKG_MT_OBJS:=}" "${RT_SAN_LDFLAGS:=}" "${RT_SAN_SKIP:=}"
if [ -n "$SUMMARY" ]; then
  LOGS=$SUMMARY
else
  for x in "$RT_SAN_SPINEL" "$RT_SAN_TIMEOUT_BIN"; do
    [ -x "$x" ] || { echo "rt-san-check: $x is not built (make rt-san-check)" >&2; exit 2; }
  done
  for x in "$RT_SAN_LIB" "$RT_SAN_MT_LIB"; do
    [ -f "$x" ] || { echo "rt-san-check: $x is not built (make rt-san-check)" >&2; exit 2; }
  done
  # the Makefile's NPROC chain, as san_check.sh
  JOBS=${RT_SAN_CHECK_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
  LOGS=$(mktemp -d "${TMPDIR:-/tmp}/spinel-rt-san-check.XXXXXX")
  # RT_SAN_KEEP=1 keeps the logs (each program's reports, by its place in
  # the list), for --summary or a look at a report
  if [ -n "${RT_SAN_KEEP-}" ]; then echo "rt-san-check: logs in $LOGS"; else trap 'rm -rf "$LOGS"' EXIT; fi
  # a run goes on past a report (the objects are built to recover); leaks
  # are the collector's to end; a huge request answers NULL, as the
  # runtime expects of malloc, instead of stopping the program; a SEGV is the
  # runtime's own to handle (a recursion past the stack's guard page raises
  # SystemStackError, which ASan's handler reported as its stack-overflow)
  export RT_SAN_ASAN="halt_on_error=0:detect_leaks=0:allocator_may_return_null=1:detect_stack_use_after_return=0:handle_segv=0:handle_sigbus=0:use_sigaltstack=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
  export RT_SAN_UBSAN="print_stacktrace=1:report_error_type=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
  export RT_SAN_LOGS=$LOGS RT_SAN_SPINEL RT_SAN_LIB RT_SAN_MT_LIB RT_SAN_TIMEOUT_BIN RT_SAN_TIMEOUT RT_SAN_CC RT_SAN_CFLAGS \
    RT_SAN_MT_DEF RT_SAN_PKG_OBJS RT_SAN_PKG_MT_OBJS RT_SAN_LDFLAGS
  # the programs, in the order their logs are numbered, and those skipped
  if [ $# -gt 0 ]; then
    printf '%s\n' "$@" > "$LOGS/list"
    : > "$LOGS/skipped"
  else
    printf '%s\n' $RT_SAN_SKIP > "$LOGS/skipped"
    : > "$LOGS/whole"
    ls test/*.rb packages/*/test/*.rb 2>/dev/null | grep -vxF -f <(cat "$LOGS/skipped"; echo /) > "$LOGS/list"
  fi
fi
total=$(wc -l < "$LOGS/list" | tr -d ' ')
[ "$total" -gt 0 ] || { echo "rt-san-check: no program to run" >&2; exit 2; }

if [ -z "$SUMMARY" ]; then
  # the number and the path travel as one NUL-ended item, as in san_check.sh
  i=0
  while IFS= read -r f; do
    i=$((i + 1))
    printf '%06d:%s\0' "$i" "$f"
  done < "$LOGS/list" | xargs -0 -n 1 -P "$JOBS" "$ROOT/tools/rt_san_check.sh" --one
  xst=${PIPESTATUS[1]}
  [ "$xst" -eq 0 ] || { echo "rt-san-check: xargs ended with status $xst, not every program was run" >&2; exit 2; }
fi
ran=$(find "$LOGS" -name '*.ran' | wc -l | tr -d ' ')
[ "$ran" -eq "$total" ] || { echo "rt-san-check: $ran of $total programs were run" >&2; exit 2; }

runname() {
  case $1 in
    d) echo default ;;
    g2) echo "SPINEL_GC_STRESS=2" ;;
  esac
}
count() { find "$LOGS" -name "$1" | wc -l | tr -d ' '; }
# The reports' sites, from one log: "site<TAB>what<TAB>tag" a line, tag 1
# for a read of the byte before an untagged literal (below). The site of
# undefined behaviour is the sanitizer's file:line; of a memory error, the
# first frame of the access in the runtime's C, a package's or the
# program's (named by its place in the list, "program C"), past the
# sanitizer's interceptors and libc. A symbolizer names a frame's file by
# its path or, as atos on macOS does, by its name alone: names are looked
# up in $LOGS/files. With recovery on, one log can hold several reports.
#
# The runtime reads the byte before a String's bytes for its tag (the
# header kinds; SPL gives a C literal 0xff there), and a C literal passed
# without one has the byte before its global read: by the readers'
# design (sp_exc_class_name takes "no header" to mean a literal), but
# outside the object all the same. Those reads are listed as sites of
# their own, with the file the literal is in, and counted apart.
sites_of() {
  awk -v MAP="$LOGS/files" -v TAB="$TAB" '
    BEGIN { while ((getline l < MAP) > 0) { split(l, a, "\t"); path[a[1]] = a[2] } }
    function where(file, line,   b) {
      b = file; sub(/.*\//, "", b)
      if (b ~ /^[0-9][0-9][0-9][0-9][0-9][0-9]\.[ds]\.c$/) return "program C"
      return (b in path) ? path[b] ":" line : ""
    }
    function flush() {
      if (kind == "") return
      if (site == "") { site = "no frame in the runtime or the program"; fn = "unknown" }
      if (site == "program C") site = "program C (" fn ")"
      w = kind " in " fn
      if (tag) w = w ", the byte before an untagged literal in " lit
      print site TAB w TAB tag
      kind = ""
    }
    /: runtime error: / {
      if (match($0, /[^ ]*\.[ch]:[0-9]+:[0-9]+: runtime error: /)) {
        hd = substr($0, RSTART, RLENGTH); msg = substr($0, RSTART + RLENGTH)
        split(hd, h, ":"); st = where(h[1], h[2]); if (st == "program C") st = "program C"
        if (st == "") st = h[1] ":" h[2]
        print st TAB msg TAB 0
      }
      next
    }
    /ERROR: AddressSanitizer: / {
      flush(); kind = $0; sub(/.*ERROR: AddressSanitizer: /, "", kind); sub(/ .*/, "", kind)
      site = ""; fn = ""; tag = 0; lit = ""; stack = 1; seen = 0; next
    }
    stack && /^ *#[0-9]+ 0x/ {
      seen = 1
      if (site == "" && match($0, / in [A-Za-z_0-9.$]+ [^ ]*\.[ch]:[0-9]+/)) {
        fr = substr($0, RSTART + 4, RLENGTH - 4); split(fr, x, " "); split(x[2], y, ":")
        st = where(y[1], y[2]); if (st != "") { site = st; fn = x[1] }
      }
      next
    }
    stack && seen && /^ *$/ { stack = 0 }
    kind == "global-buffer-overflow" && /is located 1 bytes before global variable/ {
      tag = 1; lit = $0; sub(/.* defined in .\/?/, "", lit); sub(/\047.*/, "", lit); sub(/:.*/, "", lit)
      sub(/.*\//, "", lit)
      if (lit ~ /^[0-9][0-9][0-9][0-9][0-9][0-9]\.[ds]\.c$/) lit = "the program C"
      else if (lit in path) lit = path[lit]
    }
    /^SUMMARY: AddressSanitizer/ { flush() }
    /^SPINEL_GC_STRESS: the mark reached a freed slot/ { gc = 1 }
    gc && /phase = / { ph = $0; sub(/.*phase = /, "", ph); sub(/ .*/, "", ph); print "GC stress check (phase " ph ")" TAB "the mark reached a freed slot" TAB 0; gc = 0 }
    END { flush() }
  ' "$1" | sort -u
}
TAB=$(printf '\t')
for x in lib/*.[ch] lib/regexp/*.[ch] packages/*/*.[ch]; do printf '%s\t%s\n' "${x##*/}" "$x"; done > "$LOGS/files"
# one line per program, run and site, "site<TAB>program<TAB>run<TAB>what<TAB>tag<TAB>log"
i=0
while IFS= read -r f; do
  i=$((i + 1))
  key=$(printf %06d "$i")
  for r in d g2; do
    log=$LOGS/$key.$r.report
    [ -f "$log" ] || continue
    sites_of "$log" | while IFS="$TAB" read -r site what tag; do
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$site" "$f" "$r" "$what" "$tag" "$log"
    done
  done
done < "$LOGS/list" > "$LOGS/sites"
# a program counts as reported unless every site it reached is a read of an
# untagged literal's tag byte
bad=$(awk -F"$TAB" '$5 == 0 { print $2 }' "$LOGS/sites" | sort -u | wc -l | tr -d ' ')
tagonly=$(awk -F"$TAB" '$5 == 1 { t[$2] = 1 } $5 == 0 { b[$2] = 1 } END { n = 0; for (p in t) if (!(p in b)) n++; print n }' "$LOGS/sites")
# a site is its place and what happened there, the numbers in the message
# aside (one line for every misaligned address a load at one place reads)
awk -F"$TAB" -v OFS="$TAB" '{ w = $4; gsub(/0x[0-9a-f]+|-?[0-9]+/, "N", w); print $1 " " w, $0 }' "$LOGS/sites" > "$LOGS/keyed"
cut -f1,3 "$LOGS/keyed" | sort -u | cut -f1 | sort | uniq -c | sort -rn | while read -r n key; do
  first=$(awk -F"$TAB" -v k="$key" '$1 == k { print; exit }' "$LOGS/keyed" | cut -f2-)
  site=$(printf %s "$first" | cut -f1)
  prog=$(printf %s "$first" | cut -f2)
  r=$(runname "$(printf %s "$first" | cut -f3)")
  [ "$n" -eq 1 ] && where="$prog, $r" || where="$n programs, first $prog, $r"
  what=$(printf %s "$first" | cut -f4)
  echo "rt-san-check: $site: $what ($where)"
  if [ "$VERBOSE" -eq 1 ]; then
    log=$(printf %s "$first" | cut -f6)
    sed -n '1,60p' "$log"
  fi
done
left=$(count '*.left'); nobuild=$(count '*.nobuild'); tmo=$(count '*.timeout'); diff=$(count '*.diff'); sanfail=$(count '*.sanfail')
echo "rt-san-check: $total programs, $bad with a report, $tagonly more with only reads of an untagged literal's tag byte"
echo "rt-san-check: without a report: $diff runs answered other than .expected, $tmo ran past ${RT_SAN_TIMEOUT}s of CPU, $sanfail ended by a sanitizer's own failure; $left compiles refused, $nobuild C files did not build"
if [ "$VERBOSE" -eq 1 ]; then
  for k in diff timeout sanfail left; do
    for x in "$LOGS"/*."$k"; do [ -f "$x" ] && echo "rt-san-check: $k: $(head -n 1 "$x")"; done
  done
fi
for x in "$LOGS"/*.nobuild; do
  [ -f "$x" ] || continue
  echo "rt-san-check: did not build: $(head -n 1 "$x"): $(sed -n 2p "$x")"
done
nskip=$(grep -c . "$LOGS/skipped")
if [ -f "$LOGS/whole" ]; then
  [ "$nskip" -eq 0 ] || echo "rt-san-check: skipped $nskip programs make test leaves out on this host and mode: $(tr '\n' ' ' < "$LOGS/skipped")"
  echo "rt-san-check: not run: benchmark/*.rb and optcarrot, which make test does not run either (san-check compiles them)"
fi
[ "$bad" -eq 0 ]
