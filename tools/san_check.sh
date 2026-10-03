#!/usr/bin/env bash
# san_check.sh -- compile the corpus with the compiler built under
# AddressSanitizer and UndefinedBehaviorSanitizer and list what they report.
#
#   tools/san_check.sh [-v] [file.rb ...]
#
# The compiler frees and reuses its own tables while its memos point into
# them. A memo that outlives what it points at reads
# freed memory, and the compile still finishes: what it read is usually the
# old bytes, so the C is the same and no test notices, until the allocator
# hands the block to something else. This runs every program of the corpus
# (test/, benchmark/, packages/*/test/ and optcarrot; or the files named)
# through build/spinel-san with -c, where such a read stops the compile with
# a report, and so does undefined behaviour in the compiler's own arithmetic.
#
# One line a site: the sanitizer's file:line for undefined behaviour, the
# first frame in a .c file for a memory error, with the number of programs
# that reach it and the first of them. -v adds that program's report.
# A program the compiler leaves without C and without a report (a refusal, a
# syntax error) is no finding; the last line counts them and -v names them.
# Leaks are not reported: the compiler frees little before it exits.
#
# Exit status: 0 no report, 1 some program reported, 2 infrastructure error
# (the compiler is not built, there is no program, xargs did not run them
# all, or the compiler ended on some program with a status above 128, a
# signal's, and no report).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SP=$ROOT/build/spinel-san
[ -x "$SP" ] || { echo "san-check: build build/spinel-san first (make san-check)" >&2; exit 2; }
VERBOSE=0
[ "${1-}" = "-v" ] && { VERBOSE=1; shift; }
# the Makefile's NPROC chain: nproc is GNU, macOS answers through sysctl
JOBS=${SAN_CHECK_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
LOGS=$(mktemp -d "${TMPDIR:-/tmp}/spinel-san-check.XXXXXX")
trap 'rm -rf "$LOGS"' EXIT
# The instrumented frames are several times the plain ones, and codegen
# recurses once per nesting level of the program.
ulimit -s unlimited 2>/dev/null || ulimit -s 1048576 2>/dev/null || true
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

list() {
  if [ $# -gt 0 ]; then printf '%s\n' "$@"; return; fi
  ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  [ -f build/optcarrot-single.rb ] && echo build/optcarrot-single.rb
}

# wc -l pads its count on BSD: the counts go into the last line bare
total=$(list "$@" | wc -l | tr -d ' ')
[ "$total" -gt 0 ] || { echo "san-check: no program to compile" >&2; exit 2; }

# one log per program: a program the compiler refuses is not a finding, a
# sanitizer's report is, whatever the exit status. A compiler killed by a
# signal with no report is neither: it did not compile the program, and
# what it left is kept as .died (the status and the program, then its
# output). Any other failing status with no report is kept the same way as
# .left: the summary counts those, since no text tells a refusal from a
# failure to parse or to write (a refusal may end with its count or with
# one message alone). The files are named by the program's place in the
# list, not by its path: test/a_b.rb and test/a/b.rb, or one program named
# twice, would share a name made from the path, and the later compile
# would take the earlier one's report with it. The number and the path
# travel as one NUL-ended item: BSD xargs ends an argument at a blank
# unless -0 is given, and joins a line's pieces again with one space
i=0
list "$@" | while IFS= read -r f; do
  i=$((i + 1))
  printf '%06d:%s\0' "$i" "$f"
done | xargs -0 -n 1 -P "$JOBS" sh -c '
  key=${3%%:*}
  f=${3#*:}
  "$1" -c --no-line-map "$f" -o "$2/$key.c" > "$2/$key.log" 2>&1
  st=$?
  : > "$2/$key.ran"
  rm -f "$2/$key.c"
  grep -q "runtime error:\|ERROR: AddressSanitizer" "$2/$key.log" && exit 0
  kind=left
  [ "$st" -le 128 ] || kind=died
  [ "$st" -eq 0 ] || { echo "$st $f"; cat "$2/$key.log"; } > "$2/$key.$kind"
  rm -f "$2/$key.log"
' _ "$SP" "$LOGS"
# the command above answers 0 for every program, so anything else is xargs
# not starting, or one of its commands killed: no log then means no
# compile, not no report
xst=${PIPESTATUS[2]}
[ "$xst" -eq 0 ] || { echo "san-check: xargs ended with status $xst, not every program was compiled" >&2; exit 2; }
# and every program leaves a mark when its compile has been tried: a
# dispatch that ran fewer than the list holds has checked less than it says
ran=$(find "$LOGS" -name '*.ran' | wc -l | tr -d ' ')
[ "$ran" -eq "$total" ] || { echo "san-check: $ran of $total programs were run" >&2; exit 2; }

bad=$(find "$LOGS" -name '*.log' | wc -l | tr -d ' ')
if [ "$bad" -gt 0 ]; then
  # one line per program and site, "site<TAB>program<TAB>what<TAB>log": the
  # sanitizer's own file:line for undefined behaviour, the first frame in a
  # .c file for a memory error (the frames above it are inlined helpers)
  TAB=$(printf '\t')
  i=0
  list "$@" | while IFS= read -r f; do
    i=$((i + 1))
    log=$LOGS/$(printf %06d "$i").log
    [ -f "$log" ] || continue
    grep -o 'src/[a-z_0-9]*\.[ch]:[0-9]*:[0-9]*: runtime error: .*' "$log" |
      sed -e "s|^\(src/[^:]*:[0-9]*\):[0-9]*: runtime error: |\1$TAB$f$TAB|" -e "s|\$|$TAB$log|" | sort -u -t"$TAB" -k1,1
    if grep -q 'ERROR: AddressSanitizer' "$log"; then
      kind=$(grep -m1 -o 'ERROR: AddressSanitizer: [a-z-]*' "$log" | sed 's/.*: //')
      grep -m1 -o ' in [A-Za-z_0-9]* src/[a-z_0-9]*\.c:[0-9]*' "$log" |
        sed "s|^ in \([^ ]*\) \(.*\)|\2$TAB$f$TAB$kind in \1$TAB$log|"
    fi
  done > "$LOGS/sites"
  cut -f1 "$LOGS/sites" | sort | uniq -c | sort -rn | while read -r n site; do
    first=$(grep -m1 "^$site$TAB" "$LOGS/sites")
    prog=$(printf %s "$first" | cut -f2)
    [ "$n" -eq 1 ] && where="$prog" || where="$n programs, first $prog"
    what=$(printf %s "$first" | cut -f3)
    echo "san-check: $site: $what ($where)"
    if [ "$VERBOSE" -eq 1 ]; then
      log=$(printf %s "$first" | cut -f4)
      if printf %s "$what" | grep -q '^[a-z-]* in [A-Za-z_0-9]*$'; then
        sed -n '/ERROR: AddressSanitizer/,/^SUMMARY/p' "$log"
      else
        grep -F -A8 "$site:" "$log" | sed -n '2,/^$/p'
      fi
    fi
  done
fi
left=$(find "$LOGS" -name '*.left' | wc -l | tr -d ' ')
if [ "$left" -eq 0 ]; then
  echo "san-check: $total programs, $bad with a report"
else
  hint=" (no report; -v names them)"
  [ "$VERBOSE" -eq 1 ] && hint=
  echo "san-check: $total programs, $bad with a report, $left not compiled$hint"
  if [ "$VERBOSE" -eq 1 ]; then
    for d in "$LOGS"/*.left; do
      read -r st f < "$d"
      echo "san-check: $f: not compiled, status $st: $(sed 1d "$d" | tail -n 1)"
    done
  fi
fi
died=0
for d in "$LOGS"/*.died; do
  [ -f "$d" ] || continue
  died=1
  read -r st f < "$d"
  echo "san-check: $f: the compiler died with status $st and no report" >&2
  [ "$VERBOSE" -eq 1 ] && sed 1d "$d" >&2
done
[ "$died" -eq 0 ] || exit 2
[ "$bad" -eq 0 ]
