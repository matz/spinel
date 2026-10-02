#!/bin/sh
# result_cache.sh -- the gate's result cache, keyed by the generated C.
#
# The corpus (Makefile RUN_ONE_TEST) and the ruby/spec retention gate
# (tools/rubyspec/run.sh) always run spinel: the C it emits is how they learn
# that nothing changed. What this file lets them skip is the rest -- the C
# compile and the run -- when every input that decides the outcome is the
# same as in a run that passed. The reasoning, and what is deliberately out of
# scope, is in the Makefile's comment above RUN_ONE_TEST.
#
#   fp FILE... [-- STRING...]
#       The fingerprint of a configuration: the content of each FILE (a
#       missing one counts as missing), every header under lib/ and packages/,
#       the C compiler's identity ($RC_CC --version and -dumpmachine), the
#       loader paths, and each STRING. Computed once per run.
#   key SRC CFILE IDENT [SIDECAR...]
#       Print the cache key of one program, or print nothing and exit 1 when
#       the program must not be cached (GATE_CACHE=0, nocache below, or an
#       IDENT that does not start with a 64-digit fingerprint). The
#       key covers the result-cache format version, IDENT (the caller's
#       fingerprint and its compile line), the C text, the Ruby source, and
#       each SIDECAR's content (an absent sidecar is recorded as absent, so
#       adding one changes the key).
#   nocache SRC
#       Exit 0 when SRC must always run: it carries `# spinel: no-cache`, or
#       it or a file it requires reaches the clock, the environment, the
#       random source, threads/processes/signals, files and directories, the
#       network, or the GC's own counters (see NC_WORDS).
#   get KEY            print a stored result and exit 0, or exit 1
#   put KEY            store stdin as KEY's result (temp file, then rename)
#   prune [DAYS]       drop entries not used for DAYS days (default 14)
#
# Only passes are ever stored; the callers never `put` anything else.
set -u
RC_DIR=${RESULT_CACHE_DIR:-build/result-cache}
# Bump when what an entry means changes: the key's layout, or what the
# callers store under it.
RC_FORMAT=1
LC_ALL=C
export LC_ALL

rc_sha() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

# Identifiers whose use makes a program's outcome depend on something outside
# its key, or on chance: the clock, the environment, randomness, concurrency
# and other processes, the file system (a fixture a commit edits would not
# change the C), the network and the collector's counters. Matched as whole
# words in code lines (comment lines are skipped), so it errs towards
# no-cache: a method that merely shares a name (`sample`, `trap`) also opts
# its test out. load/open count only as receiverless calls (`Marshal.load`
# is not a file read).
NC_WORDS='Time|Process|Benchmark|Timeout|Date|DateTime|clock_gettime|sleep|ENV|Etc|getenv|isatty|tty|console|winsize|rand|srand|Random|SecureRandom|shuffle|sample|Thread|Queue|SizedQueue|Mutex|Monitor|ConditionVariable|Ractor|fork|spawn|system|exec|popen|Open3|Signal|trap|wait_readable|wait_writable|io_wait|scheduler|File|IO|Dir|FileUtils|Pathname|__dir__|Tempfile|Tmpdir|ARGF|DATA|GC|ObjectSpace|WeakRef|WeakMap|define_finalizer|Socket|TCPServer|TCPSocket|UDPSocket|UNIXSocket|UNIXServer|Addrinfo|Net|OpenSSL|Resolv'
NC_OTHER='(^|[^.[:alnum:]_])(load|open)[ (]|\$\$|`|%x|_nonblock'

# nocache_one FILE: 0 when FILE itself opts out
nocache_one() {
  grep -q '^# spinel: no-cache' "$1" && return 0
  grep -v '^[[:space:]]*#' "$1" | grep -qwE "$NC_WORDS" && return 0
  grep -v '^[[:space:]]*#' "$1" | grep -qE "$NC_OTHER" && return 0
  return 1
}

# nocache SRC: SRC and, transitively, every local file it requires. A path
# that resolves to no file here (a package, a builtin) is the package's own
# code, compiled into the C.
nocache() {
  todo=$1; seen=" "; n=0
  while [ -n "$todo" ]; do
    f=${todo%%"
"*}
    if [ "$f" = "$todo" ]; then todo=; else todo=${todo#*"
"}; fi
    case "$seen" in *" $f "*) continue;; esac
    seen="$seen$f "; n=$((n + 1))
    [ $n -gt 200 ] && return 0            # a require web this big: just run it
    nocache_one "$f" && return 0
    d=$(dirname "$f")
    for r in $(sed -nE "s/^[[:space:]]*(require_relative|require|load)[[:space:](]*[\"']([^\"']+)[\"'].*/\\1:\\2/p" "$f"); do
      p=${r#*:}; base=$d
      # a bare `require "x"` searches the load path (test/ in the corpus),
      # not the requiring file's directory
      case "$r" in require:.*|require_relative:*|load:*) ;; require:*) base=test;; esac
      for c in "$base/$p" "$base/$p.rb"; do
        if [ -f "$c" ]; then todo="$todo${todo:+
}$c"; break; fi
      done
    done
  done
  return 1
}

case "${1:-}" in
fp)
  shift
  files=; strs=
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; strs="$*"; break; fi
    files="$files $1"; shift
  done
  {
    echo "format $RC_FORMAT"
    for f in $files; do
      if [ -f "$f" ]; then rc_sha "$f"; else echo "absent $f"; fi
    done
    find lib packages -type f \( -name '*.h' -o -name '*.inc' -o -name '*.def' \) 2>/dev/null | sort | while read -r h; do rc_sha "$h"; done
    cc=${RC_CC:-cc}
    $cc --version 2>&1
    $cc -dumpmachine 2>&1
    uname -sm
    echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} LIBRARY_PATH=${LIBRARY_PATH:-}"
    echo "$strs"
  } | rc_sha | cut -c1-64
  ;;
key)
  [ "${GATE_CACHE:-1}" = 0 ] && exit 1
  src=$2; cfile=$3; ident=$4; shift 4
  [ -f "$src" ] && [ -f "$cfile" ] || exit 1
  # IDENT starts with the fingerprint; a fingerprint that failed to compute
  # would key every program on the C alone, so no key at all then
  fp=${ident%%|*}
  case "$fp" in *[!0-9a-f]*) exit 1;; esac
  [ ${#fp} -eq 64 ] || exit 1
  nocache "$src" && exit 1
  {
    echo "format $RC_FORMAT"
    echo "$ident"
    rc_sha "$cfile" | cut -c1-64
    rc_sha "$src" | cut -c1-64
    for s in "$@"; do
      if [ -f "$s" ]; then rc_sha "$s"; else echo "absent $s"; fi
    done
  } | rc_sha | cut -c1-64
  ;;
nocache)
  nocache "$2"
  ;;
get)
  [ -f "$RC_DIR/$2" ] || exit 1
  touch "$RC_DIR/$2" 2>/dev/null
  cat "$RC_DIR/$2"
  ;;
put)
  mkdir -p "$RC_DIR"
  tmp="$RC_DIR/.tmp.$$.$2"
  cat > "$tmp" && mv -f "$tmp" "$RC_DIR/$2"
  ;;
prune)
  [ -d "$RC_DIR" ] && find "$RC_DIR" -type f -mtime +"${2:-14}" -delete 2>/dev/null
  exit 0
  ;;
*)
  echo "usage: $0 fp|key|nocache|get|put|prune ..." >&2; exit 2
  ;;
esac
