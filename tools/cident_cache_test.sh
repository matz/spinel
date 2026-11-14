#!/usr/bin/env bash
# Exercise cident's cache with a tiny repository and a compiler that copies
# its input. No compiler build or full corpus is needed for these checks.
set -eu
# A commit hook exports the parent repository's Git environment. The
# fixture must own its index and refs even when the test runs in a hook.
for name in $(git rev-parse --local-env-vars); do unset "$name"; done
SOURCE=${CIDENT_TEST_SCRIPT:-$(pwd)/tools/cident.sh}
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-cident-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/tree/tools" "$T/tree/bin" "$T/tree/test/support" "$T/tree/benchmark" "$T/tree/packages/p/test" "$T/mock"
cp "$SOURCE" "$T/tree/tools/cident.sh"
cat > "$T/tree/bin/spinel" <<'COMPILER'
#!/usr/bin/env bash
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out=$2; shift;; *.rb) input=$1;; esac
  shift
done
# a program reads the support file it names, as a require_relative does
if grep -q 'support/input' "$input"; then cat "$input" test/support/input.txt > "$out"; else cat "$input" > "$out"; fi
# CIDENT_TEST_LITERAL: a frozen literal of the program's own path, as
# __FILE__ gives one, whose header holds the FNV-1a hash of its bytes
# (fzl_emit_defs). The hash is computed here apart from cident's. =wrong:
# the reference's compiler (not this tree's) writes a hash one off.
if [ -n "${CIDENT_TEST_LITERAL-}" ]; then
  off=0
  [ "$CIDENT_TEST_LITERAL" = wrong ] && [ "$0" != "$CIDENT_TEST_TMP/tree/bin/spinel" ] && off=1
  perl -e 'my ($p, $off, $h) = @ARGV; my $n = length $p;
    { use integer; $h = -3750763034362895579; $h = ($h ^ $_) * 1099511628211 for unpack "C*", $p; $h += $off }
    printf "static struct { sp_str_hdr h; unsigned char m; char d[%d]; } _fzl_0 = { { 0, %d | SP_STR_SIZE_ASCII7, %d, 0x%xULL }, 0xf1, \"%s\" };\n",
      $n + 1, $n + 1, $n, $h || 1, $p' "$PWD/$input" "$off" >> "$out"
fi
COMPILER
chmod +x "$T/tree/bin/spinel"
printf 'puts 1 # support/input\n' > "$T/tree/test/a.rb"
printf 'support one\n' > "$T/tree/test/support/input.txt"
touch "$T/tree/benchmark/.keep" "$T/tree/packages/p/test/.keep"
cat > "$T/mock/make" <<'MAKE'
#!/usr/bin/env bash
: > "$CIDENT_TEST_TMP/build-$$"
if [ -f "$CIDENT_TEST_TMP/hold" ]; then
  while [ -f "$CIDENT_TEST_TMP/hold" ]; do sleep 0.05; done
fi
MAKE
# Keep the existing GNU sed invocation portable in this isolated fixture;
# macOS support is tested separately by cident's own portability change.
cat > "$T/mock/sed" <<'SED'
#!/usr/bin/env bash
if [ "${1-}" = -i ]; then
  "$CIDENT_TEST_SED" "$2" "$3" > "$3.tmp" && mv "$3.tmp" "$3"
else exec "$CIDENT_TEST_SED" "$@"; fi
SED
chmod +x "$T/mock/"*
export CIDENT_TEST_TMP=$T CIDENT_TEST_SED=$(command -v sed)
export PATH="$T/mock:$PATH" CIDENT_JOBS=1
cd "$T/tree"
git init -q
git add .
git -c user.name=Test -c user.email=test@example.invalid commit -qm 'Fixture'
run() { bash tools/cident.sh HEAD > "$T/out" 2>&1; }
# Reap a background run within 30 s; a run still alive then is killed and
# reported, so a locking regression fails the test instead of hanging it.
reap() {
  for ((i=0; i<600; i++)); do kill -0 "$1" 2>/dev/null || break; sleep 0.05; done
  if kill -0 "$1" 2>/dev/null; then kill -KILL "$1"; wait "$1" 2>/dev/null; return 1; fi
  wait "$1" 2>/dev/null; return 0
}
check() {
  if run && grep -q '0 differ, 0 refusal changes.*0 not in the reference' "$T/out"; then
    echo "$1: identical"
  else echo "$1: stale"; fi
}
if [ "$1" = corpus ]; then
  check cold
  check warm
  printf 'puts 2 # support/input\n' > test/a.rb
  check edited
  printf 'puts 3\n' > test/b.rb
  check added
  printf 'support two\n' > test/support/input.txt
  check dependency
  rm test/b.rb
  check removed
  printf 'builds: '; find "$T" -name 'build-*' | wc -l | tr -d ' '
elif [ "$1" = literal ]; then
  # The rename of the reference tree's path reaches the hash in the header
  # of a literal that holds the path, not only its bytes.
  export CIDENT_TEST_LITERAL=1
  check literal
  # a hash the reference's compiler got wrong is not renamed into a right one
  # (a new reference: the cached one holds the right hash)
  rm -rf build/cident; CIDENT_TEST_LITERAL=wrong
  check 'wrong hash'
elif [ "$1" = abandoned ]; then
  # A run killed with SIGKILL runs no cleanup. The next run must still get
  # the lock once the killed run's own processes are gone.
  touch "$T/hold"
  bash tools/cident.sh HEAD > "$T/first" 2>&1 & first=$!
  for ((i=0; i<200; i++)); do
    [ "$(find "$T" -name 'build-*' | wc -l)" -ge 1 ] && break
    sleep 0.05
  done
  kill -KILL "$first"; wait "$first" 2>/dev/null || :
  rm "$T/hold"
  bash tools/cident.sh HEAD > "$T/second" 2>&1 & second=$!
  if ! reap "$second"; then
    echo "second: blocked"
  elif grep -q '1 identical, 0 differ, 0 refusal changes' "$T/second"; then
    echo "second: identical"
  else echo "second: incomplete"; fi
else
  touch "$T/hold"
  bash tools/cident.sh HEAD > "$T/first" 2>&1 & first=$!
  for ((i=0; i<200; i++)); do
    [ "$(find "$T" -name 'build-*' | wc -l)" -ge 1 ] && break
    sleep 0.05
  done
  bash -x tools/cident.sh HEAD > "$T/second" 2>&1 & second=$!
  for ((i=0; i<200; i++)); do
    if grep -qs '+ cident_lock' "$T/second" || [ "$(find "$T" -name 'build-*' | wc -l)" -ge 2 ]; then break; fi
    sleep 0.05
  done
  rm "$T/hold"
  blocked=
  reap "$first" || blocked="$blocked first"
  reap "$second" || blocked="$blocked second"
  printf 'builds: '; find "$T" -name 'build-*' | wc -l | tr -d ' '
  for result in first second; do
    if [[ " $blocked " == *" $result "* ]]; then echo "$result: blocked"
    elif grep -q '1 identical, 0 differ, 0 refusal changes' "$T/$result"; then
      echo "$result: identical"
    else echo "$result: incomplete"; fi
  done
fi
