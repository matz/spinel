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
#   pch-key CC VERSION INCLUDE_DIR FLAG...
#       Hash the compiler command, version, any managed include directory
#       and ordered arguments of a PCH build.
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
#   lookup KEY_OUT RESULT_OUT SRC CFILE IDENT [SIDECAR...]
#       Write an eligible key to KEY_OUT. On a cached PASS, also write its
#       payload to RESULT_OUT and succeed. A miss exits 1.
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

# Hash a sorted NUL-delimited file list without launching a hasher per path.
rc_sha_paths() {
  if command -v sha256sum >/dev/null 2>&1; then
    xargs -0 sha256sum
  else
    xargs -0 shasum -a 256
  fi
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
  # Drain the pipe so the comment filter can finish writing large sources.
  grep -v '^[[:space:]]*#' "$1" | grep -wE "$NC_WORDS" >/dev/null && return 0
  grep -v '^[[:space:]]*#' "$1" | grep -E "$NC_OTHER" >/dev/null && return 0
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

rc_key_file_hash() {
  case "$1" in
    *'
'*)
      # Some sha256sum implementations print these names on multiple
      # lines. Keep the prior per-line key input.
      file_hash=$(rc_sha "$1" && printf .) || return 1
      printf '%s' "${file_hash%.}" | cut -c1-64;;
    *)
      file_hash=$(rc_sha "$1") || return 1
      printf '%.64s\n' "$file_hash";;
  esac
}

rc_key() {
  [ "${GATE_CACHE:-1}" = 0 ] && return 1
  src=$1; cfile=$2; ident=$3; shift 3
  [ -f "$src" ] && [ -f "$cfile" ] || return 1
  # IDENT starts with the fingerprint; a fingerprint that failed to compute
  # would key every program on the C alone, so no key at all then
  fp=${ident%%|*}
  case "$fp" in *[!0-9a-f]*) return 1;; esac
  [ ${#fp} -eq 64 ] || return 1
  nocache "$src" && return 1
  # Send the producer's success marker around the pipe. The digest alone
  # cannot distinguish a complete input from a failed component hash.
  digest=$(
    {
      {
        echo "format $RC_FORMAT"
        echo "$ident"
        for f in "$cfile" "$src"; do
          rc_key_file_hash "$f" || exit 1
        done
        for s in "$@"; do
          if [ -f "$s" ]; then rc_sha "$s" || exit 1; else echo "absent $s"; fi
        done
        printf 'ok\n' >&3
      } | rc_sha
    } 3>&1
  ) || return 1
  case "$digest" in
    'ok
'*) printf '%.64s\n' "${digest#*'
'}";;
    *) return 1;;
  esac
}

rc_get() {
  [ -f "$RC_DIR/$1" ] || return 1
  touch "$RC_DIR/$1" 2>/dev/null
  cat "$RC_DIR/$1"
}

case "${1:-}" in
pch-key)
  shift
  include_files=
  if [ -n "$3" ]; then
    if command -v sha256sum >/dev/null 2>&1; then
      include_files=$(find -L "$3" -type f -exec sha256sum {} +) || exit 1
    else
      include_files=$(find -L "$3" -type f -exec shasum -a 256 {} +) || exit 1
    fi
  fi
  {
    printf '%s\0' "$@"
    if [ -n "$include_files" ]; then printf '%s\n' "$include_files" | sort; fi
  } | rc_sha | cut -c1-64
  ;;
fp)
  shift
  files=; strs=
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then shift; strs="$*"; break; fi
    files="$files $1"; shift
  done
  fp_dir=$(mktemp -d "${TMPDIR:-/tmp}/spinel-result-fp.XXXXXX") || exit 1
  trap 'rm -rf "$fp_dir"' EXIT
  # A partial manifest must not become a valid key when a tool fails.
  find lib packages -type f \( -name '*.h' -o -name '*.inc' -o -name '*.def' \) -print0 > "$fp_dir/paths" 2>/dev/null || exit 1
  perl -0e 'binmode STDIN; binmode STDOUT; print sort <STDIN>; close STDOUT or die $!' < "$fp_dir/paths" > "$fp_dir/sorted" || exit 1
  if [ -s "$fp_dir/sorted" ]; then
    rc_sha_paths < "$fp_dir/sorted" > "$fp_dir/headers" || exit 1
  else
    : > "$fp_dir/headers" || exit 1
  fi
  {
    echo "format $RC_FORMAT" || exit 1
    for f in $files; do
      if [ -f "$f" ]; then rc_sha "$f" || exit 1; else echo "absent $f" || exit 1; fi
    done
    cat "$fp_dir/headers" || exit 1
    cc=${RC_CC:-cc}
    $cc --version 2>&1 || exit 1
    $cc -dumpmachine 2>&1 || exit 1
    uname -sm || exit 1
    echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-} LIBRARY_PATH=${LIBRARY_PATH:-}" || exit 1
    echo "$strs"
  } > "$fp_dir/input" || exit 1
  fp_sum=$(rc_sha < "$fp_dir/input") || exit 1
  printf '%.64s\n' "$fp_sum"
  ;;
key)
  shift
  rc_key "$@"
  ;;
nocache)
  nocache "$2"
  ;;
get)
  rc_get "$2"
  ;;
lookup)
  key_out=$2; result_out=$3; shift 3
  key=$(rc_key "$@") && [ -n "$key" ] || exit 1
  printf '%s\n' "$key" > "$key_out" || exit 1
  hit=$(rc_get "$key") || exit 1
  case "$hit" in
    *'
'*) exit 1;;
    PASS|PASS"	"?*) printf '%s\n' "$hit" > "$result_out";;
    *) exit 1;;
  esac
  ;;
put)
  mkdir -p "$RC_DIR"
  tmp="$RC_DIR/.tmp.$$.$2"
  cat > "$tmp" && mv -f "$tmp" "$RC_DIR/$2"
  ;;
claim)
  mkdir -p "$RC_DIR"
  mkdir "$RC_DIR/.busy.$2" 2>/dev/null
  ;;
release)
  rmdir "$RC_DIR/.busy.$2" 2>/dev/null
  exit 0
  ;;
await)
  n=0
  while [ -d "$RC_DIR/.busy.$2" ] && [ $n -lt 600 ]; do sleep 0.1; n=$((n + 1)); done
  [ -f "$RC_DIR/$2" ] || exit 1
  cat "$RC_DIR/$2"
  ;;
prune)
  [ -d "$RC_DIR" ] && find "$RC_DIR" -type f -mtime +"${2:-14}" -delete 2>/dev/null
  [ -d "$RC_DIR" ] && find "$RC_DIR" -maxdepth 1 -type d -name '.busy.*' -mmin +60 -exec rmdir {} + 2>/dev/null
  exit 0
  ;;
*)
  echo "usage: $0 fp|key|lookup|nocache|get|put|prune ..." >&2; exit 2
  ;;
esac
