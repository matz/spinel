#!/bin/sh
# spin end-to-end check: scaffold -> path dep -> git dep -> lock -> vendor ->
# offline -> test, all hermetic (file:// git remote, scratch HOME cache).
# Usage: tools/spin_e2e.sh <spin-binary>   (run from the repo root)
set -e

SPIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=$(mktemp -d /tmp/spin-e2e.XXXXXX)
export XDG_CACHE_HOME="$WORK/cache"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "spin-e2e FAIL: $1" >&2; exit 1; }
expect() { # expect <label> <want> <got>
  [ "$3" = "$2" ] || fail "$1: want [$2] got [$3]"
}

cd "$WORK"

# --- scaffold + run -----------------------------------------------------------
"$SPIN" new app >/dev/null
cd app

# --- `<command> --help` answers with the usage and runs nothing (#4507) ---------
"$SPIN" pack --help 2>&1 | grep -q '^  pack \[name\]' || fail "pack --help: usage does not list pack"
[ ! -d build/pack ] || fail "pack --help: ran the pack"
"$SPIN" build -h 2>&1 | grep -q '^usage: spin' || fail "build -h: no usage"
[ ! -f build/bin/app ] || fail "build -h: built"

expect "scaffold run" "Hello from app" "$("$SPIN" run 2>&1 | tail -1)"

# --- the allocator is found where pkg-config says it is -------------------------
# `[package] allocator` links as a bare -l<name>, which the linker looks for
# only in its default directories; Homebrew's jemalloc on Apple Silicon lives
# in /opt/homebrew/lib, outside them, and failed to link although installed.
# spin puts pkg-config's -L directories on LIBRARY_PATH for the compile. A
# stand-in library in a directory no linker searches, and a stand-in
# pkg-config that knows it, make the case portable. The directory has a space
# in its name, which pkg-config writes escaped ("lib\ dir"), and the build
# inherits a LIBRARY_PATH with an apostrophe in it; neither may break the
# lookup or the command it ends up in.
mkdir -p "$WORK/alloc/lib dir" "$WORK/alloc/bin"
printf 'int spin_e2e_fake_allocator;\n' > "$WORK/alloc/fake.c"
${CC:-cc} -c "$WORK/alloc/fake.c" -o "$WORK/alloc/fake.o"
ar rcs "$WORK/alloc/lib dir/libspine2efakealloc.a" "$WORK/alloc/fake.o"
cat > "$WORK/alloc/bin/pkg-config" <<EOF
#!/bin/sh
[ "\$1 \$2" = "--libs-only-L spine2efakealloc" ] && { echo "-L$WORK/alloc/lib\\\\ dir"; exit 0; }
exit 1
EOF
chmod +x "$WORK/alloc/bin/pkg-config"
"$WORK/alloc/bin/pkg-config" --libs-only-L spine2efakealloc | grep -q 'lib\\ dir$' ||
  fail "allocator: stand-in pkg-config does not escape the space"
cp spin.toml spin.toml.orig
printf '\n[package]\nallocator = "spine2efakealloc"\n' >> spin.toml
"$SPIN" clean >/dev/null
if env -u LIBRARY_PATH "$SPIN" build >/dev/null 2>&1; then
  fail "allocator: linked a library nothing says where to find"
fi
PATH="$WORK/alloc/bin:$PATH" LIBRARY_PATH="$WORK/alloc/it's elsewhere" "$SPIN" build >/dev/null 2>&1 ||
  fail "allocator: not found in the directory pkg-config reports"
expect "allocator run" "Hello from app" "$(./build/bin/app 2>&1 | tail -1)"
# Not a library name: refused before anything reaches a shell.
mv spin.toml.orig spin.toml
cp spin.toml spin.toml.orig
printf '\n[package]\nallocator = "x; touch %s/alloc/ran #"\n' "$WORK" >> spin.toml
if PATH="$WORK/alloc/bin:$PATH" "$SPIN" build >"$WORK/alloc/bad.out" 2>&1; then
  fail "allocator: built with an allocator that is not a library name"
fi
grep -q 'is not a library name' "$WORK/alloc/bad.out" || fail "allocator: no message for a non-name allocator"
[ ! -e "$WORK/alloc/ran" ] || fail "allocator: a non-name allocator reached a shell"
mv spin.toml.orig spin.toml
"$SPIN" clean >/dev/null

# --- `--` with no target ahead of it names every target, not `--` itself --------
# `spin build -- --profile` passes a compiler flag and names no target. The
# flag has to reach the compiler and every bin has to build; naming `--` as the
# target is the bug (it looked for bin/--.rb and built nothing).
"$SPIN" build -- --profile >/dev/null 2>&1 || fail "build -- <flag>: exited non-zero"
[ -f build/bin/app ] || fail "build -- <flag>: built no target"
[ -f build/bin/app.symbols.json ] || fail "build -- <flag>: flag never reached the compiler"
"$SPIN" clean >/dev/null
"$SPIN" build app -- --profile >/dev/null 2>&1 || fail "build <target> -- <flag>: exited non-zero"
[ -f build/bin/app.symbols.json ] || fail "build <target> -- <flag>: flag never reached the compiler"
"$SPIN" clean >/dev/null

# --- an emit-only flag after `--` is refused, and leaves the binary alone -----
# `spin build -- -c` used to write C SOURCE over build/bin/<target>, keep its
# executable bit and exit 0, so nothing said the binary was gone.
"$SPIN" build >/dev/null 2>&1 || fail "build before emit-only check: exited non-zero"
before=$(head -c 4 build/bin/app | od -An -c | tr -d ' \n')
"$SPIN" build -- -c >/dev/null 2>&1 && fail "build -- -c: exited zero"
[ -f build/bin/app ] || fail "build -- -c: removed the binary"
after=$(head -c 4 build/bin/app | od -An -c | tr -d ' \n')
expect "build -- -c leaves the binary" "$before" "$after"
"$SPIN" clean >/dev/null

# --- library package (path dependency) --------------------------------------------
cd "$WORK"
"$SPIN" new spinel-ansi >/dev/null
rm -rf spinel-ansi/bin
printf '[package]\nname = "ansi"\n' > spinel-ansi/spin.toml
cat > spinel-ansi/ansi.rb <<'EOF'
module Ansi
  def self.red(s) = "\e[31m" + s + "\e[0m"
end
EOF

# --- git-source package (file:// remote) -------------------------------------------
mkdir gitgem
cd gitgem
git init -q
printf '[package]\nname = "greet"\n' > spin.toml
printf 'module Greet\n  def self.hi(n) = "hi " + n\nend\n' > greet.rb
git add spin.toml greet.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm init
cd "$WORK/app"

printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\n' "$WORK" > spin.toml
printf 'require "ansi"\nrequire "greet"\nputs Ansi.red(Greet.hi("spin"))\n' > bin/app.rb

expect "fetch" "fetched 2 package(s)" "$("$SPIN" fetch 2>&1 | tail -1)"
WANT_OUT=$(printf '\033[31mhi spin\033[0m')
expect "run with deps" "$WANT_OUT" "$("$SPIN" run 2>&1 | tail -1)"

# --- lock ----------------------------------------------------------------------
"$SPIN" lock >/dev/null
[ -f spin.lock ] || fail "spin.lock not written"
grep -q '^\[lock\.greet\]$' spin.lock || fail "spin.lock lacks [lock.greet]"
grep -q '^ref = "[0-9a-f]\{40\}"$' spin.lock || fail "spin.lock lacks a full-SHA ref"

# --- add / remove edit the manifest --------------------------------------------
"$SPIN" add extra --path ../spinel-ansi >/dev/null
grep -q '^extra = ' spin.toml || fail "spin add didn't edit spin.toml"
"$SPIN" remove extra >/dev/null
grep -q '^extra = ' spin.toml && fail "spin remove didn't edit spin.toml"

# --- test: snapshot regen + CRuby parity fallback (both need dep -I) ------------
printf 'require "ansi"\nputs Ansi.red("t")\n' > test/color_test.rb
expect "test (CRuby parity)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"
"$SPIN" test --regen >/dev/null 2>&1
[ -s test/color_test.rb.expected ] || fail "test --regen wrote no snapshot"
expect "test (snapshot)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"

# a snapshot has to round-trip: `test --regen` then `test` must agree. regen
# took stdout alone while the comparison takes stdout+stderr, so any program
# writing to stderr regenerated a snapshot the next run could not match (#3405).
printf '$stderr.puts "err"\nputs "out"\n' > test/streams_test.rb
"$SPIN" test --regen streams_test.rb >/dev/null 2>&1
grep -q '^err$' test/streams_test.rb.expected || fail "test --regen dropped stderr"
expect "test (regen round-trip)" "1/1 passed" "$("$SPIN" test streams_test.rb 2>&1 | tail -1)"
rm -f test/streams_test.rb test/streams_test.rb.expected

# the compiler beside spin has to be an executable FILE. A checkout of the
# compiler repo cloned next to the project is a DIRECTORY of that name, and
# spin used to try to run it (#3407).
mkdir -p "$WORK/sib/spinel"
cp "$SPIN" "$WORK/sib/spin"
printf 'puts "sib"\n' > test/sib_test.rb        # fresh: must actually compile
# What it falls through TO is the compiler on PATH, so put it there rather than
# relying on the caller's: a developer shell usually has bin/ on PATH and CI
# does not, which is the difference between this passing everywhere and only
# passing locally.
expect "test (sibling spinel dir)" "1/1 passed" "$(PATH="$(dirname "$SPIN"):$PATH" "$WORK/sib/spin" test sib_test.rb 2>&1 | tail -1)"
rm -rf "$WORK/sib"; rm -f test/sib_test.rb test/sib_test.rb.expected

# a test binary older than the compiler is stale, also when spin is run by its
# bare name through PATH. $0 is then "spin" with no directory, the compiler
# beside spin was never found, and the compiler's mtime never entered the
# freshness bound: after a compiler upgrade every test came back "(cached)",
# still the old compiler's binary. Backdating the project and the binary
# stands in for the upgrade without touching the real compiler. A project of
# its own, with no dependencies: a dependency newer than the binary would force
# the recompile by itself and hide the bug.
cd "$WORK"
"$SPIN" new stale >/dev/null
cd stale
printf 'puts "stale"\n' > test/stale_test.rb
"$SPIN" test --regen stale_test.rb >/dev/null 2>&1
"$SPIN" test stale_test.rb >/dev/null 2>&1 || fail "test (stale compiler): first run failed"
find . -path ./build -prune -o -type f -exec touch -t 200001010000 {} +
touch -t 200001010001 build/test/stale_test
OUT=$(PATH="$(dirname "$SPIN"):$PATH" spin test stale_test.rb 2>&1) || true
case "$OUT" in
  *"(cached)"*) fail "test via PATH reused a binary older than the compiler: [$OUT]" ;;
esac
expect "test via PATH (stale binary)" "1/1 passed" "$(echo "$OUT" | tail -1)"
# An empty PATH component is the current directory, to the shell and so to
# spin's which. With PATH=":<decoy>" the shell runs ./spin, whose compiler sits
# beside it. which used to skip the empty component and answer the decoy spin
# further along, and spin then built with the decoy's compiler. The decoy
# compiler here only fails, so taking it fails the test.
mkdir -p "$WORK/decoy"
cp "$SPIN" "$WORK/decoy/spin"
printf '#!/bin/sh\necho "decoy compiler ran" >&2\nexit 1\n' > "$WORK/decoy/spinel"
chmod +x "$WORK/decoy/spinel"
ln -s "$SPIN" spin
ln -s "$(dirname "$SPIN")/spinel" spinel
printf 'puts "here"\n' > test/here_test.rb      # fresh: must actually compile
printf 'here\n' > test/here_test.rb.expected
OUT=$(PATH=":$WORK/decoy:$PATH" spin test here_test.rb 2>&1) || true
case "$OUT" in
  *"decoy compiler ran"*) fail "an empty PATH component: spin took the compiler of a later spin: [$OUT]" ;;
esac
expect "test via an empty PATH component" "1/1 passed" "$(echo "$OUT" | tail -1)"
cd "$WORK/app"; rm -rf "$WORK/stale" "$WORK/decoy"

# a build that FAILS must not be reported ok by the run phase: a failed compile
# leaves the previous binary where it was, and File.exist? read that as "it
# built", so `spin test` printed the parse error and then ran the executable an
# older source had produced (#4085).
printf 'puts "first"\n' > test/stale_test.rb
printf 'first\n' > test/stale_test.rb.expected
expect "test (builds once)" "1/1 passed" "$("$SPIN" test stale_test.rb 2>&1 | tail -1)"
printf 'def broken\n  puts "first"\n' > test/stale_test.rb   # no closing end
out=$("$SPIN" test stale_test.rb 2>&1) && status=0 || status=$?
expect "test (broken build is not ok)" "0/1 passed" "$(printf '%s\n' "$out" | tail -1)"
case "$out" in *"ok   stale_test.rb"*) fail "broken build still reported ok" ;; esac
if [ "$status" = "0" ]; then fail "broken build exited 0"; fi
rm -f test/stale_test.rb test/stale_test.rb.expected

# a large test/ directory: enumerating ~60 entries allocates enough to GC
# mid-glob, which swept the unrooted result array (heap corruption before
# any child spawned, #2178)
for i in $(seq 1 60); do printf 'puts 1\n' > "test/gc$i.rb"; done
expect "test (many files)" "1/1 passed" "$("$SPIN" test color_test.rb 2>&1 | tail -1)"
rm -f test/gc*.rb

# --- carried native C (M2): package .c compiled to the shared cache, --link'ed ------
cd "$WORK"
mkdir -p spinel-fast
printf '[package]\nname = "fast"\nsources = ["*.c"]\n' > spinel-fast/spin.toml
cat > spinel-fast/fast.rb <<'EOF'
module Fast
  ffi_func :fast_quad, [:int], :int
end
EOF
cat > spinel-fast/fast_ext.c <<'EOF'
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
EOF
cd "$WORK/app"
printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\nfast = { path = "../spinel-fast" }\n' "$WORK" > spin.toml
printf 'require "ansi"\nrequire "greet"\nrequire "fast"\nputs Ansi.red(Greet.hi("spin"))\nputs Fast.fast_quad(10)\n' > bin/app.rb
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "native compile line missing"
expect "carried-C result" "40" "$(echo "$OUT" | tail -1)"
# second build: object cached, no recompile
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc " && fail "native object not reused from cache"
expect "carried-C cached result" "40" "$(echo "$OUT" | tail -1)"
# touching the .c invalidates the cached object and the app binary
sleep 1
touch ../spinel-fast/fast_ext.c
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "touched .c not recompiled"

# CC is a command line, not a path: a compiler-cache wrapper makes it two words,
# which is what CI and most developer setups pass. The object cache keys its
# directory on the compiler, so the space went straight into a path and the
# unquoted -o argument split at it.
"$SPIN" clean >/dev/null
OUT=$(CC="env ${CC:-cc}" "$SPIN" run 2>&1)
expect "carried-C with a wrapped CC" "40" "$(echo "$OUT" | tail -1)"

# --- unresolved require is a hard error (spin sets SPINEL_REQUIRE_GATE) ---------
printf 'require "nosuchgem"\nputs 1\n' > bin/broken.rb
if "$SPIN" build broken >/dev/null 2>"$WORK/gate.err"; then
  fail "unresolved require compiled anyway"
fi
grep -q "nosuchgem" "$WORK/gate.err" || fail "gate error doesn't name the missing gem"
rm -f bin/broken.rb

# --- vendor -> offline, with and without the lock -------------------------------
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME"
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (locked, vendored)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"
rm -f spin.lock
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (no lock)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"

# --- list / tree -----------------------------------------------------------------
"$SPIN" list | grep -q "^ansi 0.0.0 (path " || fail "spin list lacks the path dep"
"$SPIN" list | grep -q "^greet 0.0.0 (git " || fail "spin list lacks the git dep"
"$SPIN" list --json | grep -q '^\[{"name":"' || fail "spin list --json shape"
"$SPIN" tree | grep -q "^  fast 0.0.0" || fail "spin tree lacks the nested dep line"
"$SPIN" tree --json | grep -q '"deps":\[' || fail "spin tree --json shape"

# --- index (M3): TOML index, MVS selection, search ------------------------------
cd "$WORK"
mkdir hello
cd hello
git init -q
printf '[package]\nname = "hello"\nversion = "1.0.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v1"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
SHA1=$(git rev-parse HEAD)
printf '[package]\nname = "hello"\nversion = "1.1.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v11"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v11
SHA2=$(git rev-parse HEAD)
cd "$WORK"
mkdir -p index/packages
cd index
git init -q
printf 'name = "hello"\nrepo = "file://%s/hello"\n\n[[release]]\nversion = "1.0.0"\nref = "%s"\n\n[[release]]\nversion = "1.1.0"\nref = "%s"\n' "$WORK" "$SHA1" "$SHA2" > packages/hello.toml
git add packages
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm seed
cd "$WORK"
export SPIN_INDEX="file://$WORK/index"
"$SPIN" new idxapp >/dev/null
cd idxapp
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = ">= 1.0"\n' > spin.toml
printf 'require "hello"\nputs Hello.greet\n' > bin/idxapp.rb
expect "index MVS (lowest satisfying)" "hello v1" "$("$SPIN" run 2>&1 | tail -1)"
"$SPIN" lock >/dev/null
grep -q '^version = "1.0.0"$' spin.lock || fail "index lock lacks the selected version"
grep -q "^ref = \"$SHA1\"$" spin.lock || fail "index lock lacks the release SHA"
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = "~> 1.1"\n' > spin.toml
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "reselecting 1.1.0" || fail "constraint change didn't reselect"
expect "index reselect" "hello v11" "$(echo "$OUT" | tail -1)"
"$SPIN" search hell | grep -q "^hello 1.1.0 " || fail "spin search misses the gem"
printf '[package]\nname = "idxapp"\n\n[dependencies]\n' > spin.toml
"$SPIN" add hello --version "~> 1.0" >/dev/null
grep -q '^hello = "~> 1.0"$' spin.toml || fail "spin add --version didn't write the constraint"
"$SPIN" lock >/dev/null
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME/spin/packages" "$XDG_CACHE_HOME/spin/index"
"$SPIN" clean >/dev/null
expect "index offline (locked, vendored)" "hello v1" "$(SPIN_OFFLINE=1 "$SPIN" run 2>&1 | tail -1)"
unset SPIN_INDEX

# --- publish (M4): validations + --direct into the index -------------------------
export SPIN_INDEX="file://$WORK/index"
git -C "$WORK/index" config receive.denyCurrentBranch updateInstead
cd "$WORK"
"$SPIN" new publib --lib >/dev/null
cd publib
printf '[package]\nname = "publib"\nversion = "0.1.0"\n' > spin.toml
printf 'module Publib\n  def self.hi = "hi"\nend\n' > publib.rb
git init -q
git add spin.toml publib.rb .gitignore
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
git init -q --bare "$WORK/publib-remote.git"
git remote add origin "$WORK/publib-remote.git"
git push -q origin HEAD
git fetch -q origin
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "publish without tests must be refused"
echo "$OUT" | grep -q "publish requires tests" || fail "missing-tests message"
printf 'require "publib"\nputs Publib.hi\n' > test/hi_test.rb
git add test/hi_test.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm tests
git push -q origin HEAD
git fetch -q origin
expect "publish --direct" "published publib 0.1.0 (direct)" "$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1 | tail -1)"
grep -q '^ref = "[0-9a-f]\{40\}"$' "$WORK/index/packages/publib.toml" || fail "published entry lacks a full SHA"
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "duplicate publish must be refused"
echo "$OUT" | grep -q "already in the index" || fail "duplicate message"
# The same version spelled differently is the same version. vcmp pads the
# shorter side with zeros, so "0.1" and "0.1.0" satisfy exactly the same
# constraints; a textual duplicate check let both into the index, and the
# native object cache -- whose directory is the version AS WRITTEN -- then
# split in two. Same shape as "2026.9.8" against "2026.09.08".
sed 's/^version = "0.1.0"$/version = "0.1"/' spin.toml > spin.toml.t && mv spin.toml.t spin.toml
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qam "respell the version"
git push -q origin HEAD
git fetch -q origin
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "a respelled version must be refused as the duplicate it is"
echo "$OUT" | grep -q "which is the same version" || fail "respelled-duplicate message"
sed 's/^version = "0.1"$/version = "0.1.0"/' spin.toml > spin.toml.t && mv spin.toml.t spin.toml
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qam "restore the version"
git push -q origin HEAD
git fetch -q origin
OUT=$("$SPIN" publish --direct --repo https://example.com/other/spinel-publib 2>&1) \
  && fail "same name, different repo must be refused"
echo "$OUT" | grep -q "name policy" || fail "name-policy message"
"$SPIN" search publib | grep -q "^publib 0.1.0 " || fail "published package missing from search"

# --- R8 probes: publish records a pass; resolution warns on recorded fails -------
grep -q '^\[\[probe\]\]$' "$WORK/index/packages/publib.toml" || fail "publish wrote no probe record"
grep -q '^result = "pass"$' "$WORK/index/packages/publib.toml" || fail "probe record isn't a pass"
REV=$(cd "$WORK" && "$(dirname "$SPIN")/spinel" --version | awk '{print $3}' | tr -d '()')
printf '\n[[probe]]\nversion = "0.1.0"\nspinel = "%s"\nresult = "fail"\ndetail = "e2e-injected"\ndate = "2026-01-01"\n' "$REV" >> "$WORK/index/packages/publib.toml"
git -C "$WORK/index" add packages/publib.toml
git -C "$WORK/index" -c user.email=spin@e2e -c user.name=spin-e2e commit -qm failprobe
cd "$WORK"
"$SPIN" new probeapp >/dev/null
cd probeapp
printf '[package]\nname = "probeapp"\n\n[dependencies]\npublib = "0.1.0"\n' > spin.toml
rm -rf "$XDG_CACHE_HOME/spin/index"
OUT=$("$SPIN" fetch 2>&1 || true)
echo "$OUT" | grep -q "recorded FAILING with this compiler build" || fail "exact-build fail probe didn't warn"
unset SPIN_INDEX

# --- install: build + copy bin/ onto a prefix, uninstall removes ----------------
cd "$WORK/app"
"$SPIN" install --prefix "$WORK/binhome" >/dev/null
OUT=$("$WORK/binhome/app" | tail -2)
expect "installed binary runs" "$WANT_OUT
40" "$OUT"
"$SPIN" install --uninstall --prefix "$WORK/binhome" >/dev/null
[ -e "$WORK/binhome/app" ] && fail "uninstall left the binary"

# --- [[build]]: declared native build steps (#1820) ------------------------------
# A package vendoring a project with its own build system declares the step;
# it runs at dependent-application build time only (never at fetch), needs
# explicit consent, caches by content key, applies declared patches to a
# scratch copy, and feature-gated entries stay off by default. The same
# package works as the build root (app) and as a path dependency (lib).
export XDG_CONFIG_HOME="$WORK/config"
cd "$WORK"
mkdir -p spinel-mathx/vendor/mx spinel-mathx/patches spinel-mathx/bin
cat > spinel-mathx/spin.toml <<'EOF'
[package]
name = "mathx"
version = "0.1.0"

[[build]]
workdir   = "vendor/mx"
patches   = ["patches/*.patch"]
command   = "${CC:-cc} -O2 -c mx.c -o mx.o && ar rcs libmx.a mx.o"
artifacts = ["libmx.a"]

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -DCUDA -c mx.c -o mxc.o && ar rcs libmx-cuda.a mxc.o"
artifacts = ["libmx-cuda.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a"]
EOF
printf 'int mx_add(int a, int b) { return a + b; }\n' > spinel-mathx/vendor/mx/mx.c
cat > spinel-mathx/patches/01-bias.patch <<'EOF'
--- a/mx.c
+++ b/mx.c
@@ -1 +1 @@
-int mx_add(int a, int b) { return a + b; }
+int mx_add(int a, int b) { return a + b + 1; }
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\nend\n' > spinel-mathx/mathx.rb
printf 'require "mathx"\nputs Mathx.mx_add(20, 21)\n' > spinel-mathx/bin/mxdemo.rb

# app-as-root: unconsented build is refused with instructions
cd spinel-mathx
OUT=$("$SPIN" build 2>&1) && fail "unconsented native build must be refused"
echo "$OUT" | grep -q "declares a native build step" || fail "refusal message"
echo "$OUT" | grep -q "allow-native-build" || fail "refusal hint"
# consented: the patch applied to the scratch copy biases the sum by one
OUT=$("$SPIN" run --allow-native-build 2>&1)
expect "native app-as-root run (patched)" "42" "$(echo "$OUT" | tail -1)"
echo "$OUT" | grep -q '^native mathx:' || fail "native build step didn't run"
# the vendored tree is a read-only input: the patch never touches it
grep -q 'a + b + 1' vendor/mx/mx.c && fail "patch leaked into the vendored tree"
# feature-gated entry stays off by default: only the default artifact exists
N_ART=$(find "$XDG_CACHE_HOME/spin/native" -name 'libmx*.a' | wc -l | tr -d ' ')
expect "feature-gated artifact count" "1" "$N_ART"
# second build reuses the content-keyed cache (no native line)
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' && fail "cached native build re-ran"
expect "native cached rerun" "42" "$(echo "$OUT" | tail -1)"
# a content change moves the key and rebuilds
printf 'int mx_add(int a, int b) { return a + b; }\nint mx_two(void) { return 2; }\n' > vendor/mx/mx.c
printf -- '--- a/mx.c\n+++ b/mx.c\n@@ -1,2 +1,2 @@\n-int mx_add(int a, int b) { return a + b; }\n+int mx_add(int a, int b) { return a + b + 1; }\n int mx_two(void) { return 2; }\n' > patches/01-bias.patch
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' || fail "content change didn't rebuild the native step"
expect "native rebuild after change" "42" "$(echo "$OUT" | tail -1)"

# lib case: a consumer app pulls mathx as a path dependency. A fresh cache:
# a cached artifact skips consent by design (the consented command already
# ran on this machine), so refusal is only observable on a cold cache.
export XDG_CACHE_HOME="$WORK/cache-nb"
cd "$WORK"
mkdir -p nbconsumer/bin
printf '[package]\nname = "nbconsumer"\n\n[dependencies]\nmathx = { path = "../spinel-mathx" }\n' > nbconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_add(100, 100)\n' > nbconsumer/bin/app.rb
cd nbconsumer
# fetch never runs a native build (R2: packages compute nothing at fetch)
"$SPIN" fetch >/dev/null 2>&1
# unconsented dependent build is refused; `spin trust` records durable consent
OUT=$("$SPIN" build 2>&1) && fail "unconsented dependent native build must be refused"
"$SPIN" trust mathx | grep -q '^trusted: mathx' || fail "spin trust"
OUT=$("$SPIN" run 2>&1)
expect "native lib-dependency run (patched)" "201" "$(echo "$OUT" | tail -1)"

# consumer-side features: `spin add --features` records the enablement in the
# manifest (cargo-style; the lock stays resolution-only), the gated [[build]]
# entry runs, and its artifact joins the link line.
cd "$WORK"
printf 'int mx_cuda(void) { return 999; }\n' > spinel-mathx/vendor/mx/cuda.c
cat >> spinel-mathx/spin.toml <<'EOF'

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -c cuda.c -o cuda.o && ar rcs libmx-cu2.a cuda.o"
artifacts = ["libmx-cu2.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a", "${build.out}/libmx-cu2.a"]
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\n  ffi_func :mx_cuda, [], :int\nend\n' > spinel-mathx/mathx.rb
mkdir -p featconsumer/bin
printf '[package]\nname = "featconsumer"\n' > featconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_cuda\n' > featconsumer/bin/app.rb
cd featconsumer
"$SPIN" add mathx --path ../spinel-mathx --features cuda >/dev/null
grep -q 'features = \["cuda"\]' spin.toml || fail "spin add --features didn't record the enablement"
OUT=$("$SPIN" run 2>&1)
expect "feature-enabled native run" "999" "$(echo "$OUT" | tail -1)"
find "$XDG_CACHE_HOME/spin/native" -name 'libmx-cuda.a' | grep -q . || fail "enabled feature entry didn't build"

# --- [[build]]: a symlinked workdir must not write through into the target ------
# cp -R of a symlink source copies the LINK: every later step (patch, the
# build command) then runs inside the live tree the link points at (#2180
# destroyed a real checkout's outputs this way). The scratch copy now
# dereferences the workdir operand; the pointed-to tree stays pristine.
cd "$WORK"
mkdir -p symreal spinel-symx/vendor spinel-symx/patches spinel-symx/bin
printf 'int sx_add(int a, int b) { return a + b; }\n' > symreal/sx.c
printf 'PRECIOUS\n' > symreal/prior-output.a
ln -s "$WORK/symreal" spinel-symx/vendor/sx
cat > spinel-symx/spin.toml <<'SYMEOF'
[package]
name = "symx"
version = "0.1.0"

[[build]]
workdir   = "vendor/sx"
patches   = ["patches/*.patch"]
command   = "${CC:-cc} -O2 -c sx.c -o sx.o && ar rcs libsx.a sx.o"
artifacts = ["libsx.a"]

[native]
libs = ["${build.out}/libsx.a"]
SYMEOF
printf -- '--- a/sx.c\n+++ b/sx.c\n@@ -1 +1 @@\n-int sx_add(int a, int b) { return a + b; }\n+int sx_add(int a, int b) { return a + b + 1; }\n' > spinel-symx/patches/01-bias.patch
printf 'module Symx\n  ffi_func :sx_add, [:int, :int], :int\nend\n' > spinel-symx/symx.rb
printf 'require "symx"\nputs Symx.sx_add(20, 21)\n' > spinel-symx/bin/sxdemo.rb
cd spinel-symx
OUT=$("$SPIN" run --allow-native-build 2>&1)
expect "symlinked-workdir native run (patched)" "42" "$(echo "$OUT" | tail -1)"
# the LIVE tree behind the link stays pristine: source unpatched, no build
# droppings (.o/.a/.rej), the prior output intact
grep -q 'a + b + 1' "$WORK/symreal/sx.c" && fail "patch wrote through the symlinked workdir"
[ "$(cat "$WORK/symreal/prior-output.a")" = "PRECIOUS" ] || fail "prior output clobbered through the symlink"
LEFT=$(ls "$WORK/symreal" | sort | tr '\n' ' ')
expect "symlinked tree contents" "prior-output.a sx.c " "$LEFT"

# --- [[build]] mechanics from the guinea-pig report (#1845) ---------------------
# One package exercising: a top-level (non-vendor/) workdir excluded from
# carried-C discovery (its source hard-errors under a bare cc sweep);
# relative-path artifacts (a header staged under inc/); ${build.out} in a
# later entry's command (cross-entry inputs); dot-dir exclusion from the
# content key (a .git in the workdir must not churn the cache); and an
# ffi_lib name satisfied by the --link artifact (no -l against system paths).
cd "$WORK"
mkdir -p spinel-crossx/csrc spinel-crossx/csrc2
cat > spinel-crossx/spin.toml <<'EOF'
[package]
name = "crossx"
version = "0.1.0"

[[build]]
workdir   = "csrc"
command   = "${CC:-cc} -DCXBUILD=1 -O2 -c cx.c -o cx.o && ar rcs libcx.a cx.o && mkdir -p inc && cp cx.h inc/cx.h"
artifacts = ["libcx.a", "inc/cx.h"]
exclude   = ["devout*"]

[[build]]
workdir   = "csrc2"
command   = "${CC:-cc} -DCXBUILD=1 -O2 -I ${build.out}/inc -c cx2.c -o cx2.o && ar rcs libcx2.a cx2.o"
artifacts = ["libcx2.a"]

[native]
libs = ["${build.out}/libcx.a", "${build.out}/libcx2.a"]
EOF
printf '#ifndef CXBUILD\n#error carried-C swept a [[build]] workdir\n#endif\nint cx_val(void) { return 7; }\nlong cx_fsum(const double *d, long n) { return d && n >= 2 ? (long)(d[0] + d[1]) : -1; }\n' > spinel-crossx/csrc/cx.c
printf '#define CX_BONUS 30\n' > spinel-crossx/csrc/cx.h
printf '#ifndef CXBUILD\n#error carried-C swept a [[build]] workdir\n#endif\n#include "cx.h"\nint cx2_val(void) { return CX_BONUS + 5; }\n' > spinel-crossx/csrc2/cx2.c
printf 'module Crossx\n  ffi_lib "cx"\n  ffi_func :cx_val, [], :int\n  ffi_func :cx2_val, [], :int\n  ffi_func :cx_fsum, [:float_array, :int], :int\nend\n' > spinel-crossx/crossx.rb
mkdir -p crossapp/bin
printf '[package]\nname = "crossapp"\n\n[dependencies]\ncrossx = { path = "../spinel-crossx" }\n' > crossapp/spin.toml
printf 'require "crossx"\ndef widen(a)\n  a\nend\nwiden(["x"])\nputs Crossx.cx_fsum(widen([60.5, 4.5]), 2)\nputs Crossx.cx_val + Crossx.cx2_val\n' > crossapp/bin/app.rb
cd crossapp
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
expect "cross-entry native run" "42" "$(echo "$OUT" | tail -1)"
# a poly-collapsed float array marshals its element data (the :float_array
# twin of the #1855/#1867 int fix; 60.5+4.5=65, NULL would print -1)
echo "$OUT" | grep -q '^65$' || fail "poly-collapsed :float_array marshalled wrong data"
# a .git dropped into the workdir must not move the content key
mkdir -p ../spinel-crossx/csrc/.git
echo junk > ../spinel-crossx/csrc/.git/HEAD
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' && fail "VCS metadata churned the native cache key"
expect "dot-dir key stability" "42" "$(echo "$OUT" | tail -1)"
# ...but a SOURCE edit must move the key (the root-prune bug hashed every
# workdir as empty input, so this and the .git test above were
# indistinguishable): a comment appended to cx.c has to trigger a rebuild.
printf '/* key-moving edit */\n' >> ../spinel-crossx/csrc/cx.c
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' || fail "source edit did not move the native cache key"
expect "source-edit key movement" "42" "$(echo "$OUT" | tail -1)"
# an `exclude`d glob (a dev tree's build-output dir) must ride neither the
# key nor the scratch copy: dropping junk into it stays a cache hit
mkdir -p ../spinel-crossx/csrc/devout-cuda
echo junk > ../spinel-crossx/csrc/devout-cuda/stale.o
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' && fail "excluded glob churned the native cache key"
expect "exclude-glob key stability" "42" "$(echo "$OUT" | tail -1)"
# a [native] libs path no [[build]] entry produces must die loud (a stale
# path otherwise surfaces as undefined symbols at link time)
cp ../spinel-crossx/spin.toml ../spinel-crossx/spin.toml.bak
sed '/^libs/s|libcx2.a"\]|libcx2.a", "${build.out}/libnope.a"]|' ../spinel-crossx/spin.toml.bak > ../spinel-crossx/spin.toml
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1) && fail "unproduced [native] libs path did not die"
echo "$OUT" | grep -q 'not produced by any' || fail "unproduced-libs diagnostic missing: $OUT"
mv ../spinel-crossx/spin.toml.bak ../spinel-crossx/spin.toml

# a threaded program takes the package's `_mt` variant: one [[build]] entry
# produces both, `[native] libs` names the plain one, and the compiler prefers
# `<stem>_mt.<ext>` beside every link input (#4032).
cd "$WORK"
mkdir -p spinel-tvar/vendor/tv spinel-tvar/bin
cat > spinel-tvar/spin.toml <<'EOF'
[package]
name = "tvar"
version = "0.1.0"

[[build]]
workdir   = "vendor/tv"
command   = "${CC:-cc} -O2 -c tv.c -o tv.o && ${CC:-cc} -O2 -DSP_THREADS -ftls-model=initial-exec -c tv.c -o tv_mt.o"
artifacts = ["tv.o", "tv_mt.o"]

[native]
libs = ["${build.out}/tv.o"]
EOF
cat > spinel-tvar/vendor/tv/tv.c <<'EOF'
#ifdef SP_THREADS
int tv_variant(void) { return 2; }
#else
int tv_variant(void) { return 1; }
#endif
EOF
printf 'module Tvar
  ffi_func :tv_variant, [], :int
end
' > spinel-tvar/tvar.rb
printf 'require "tvar"
puts Tvar.tv_variant
' > spinel-tvar/bin/plain.rb
printf 'require "tvar"
t = Thread.new { Tvar.tv_variant }
puts t.value
' > spinel-tvar/bin/threaded.rb
cd spinel-tvar
OUT=$("$SPIN" run plain --allow-native-build 2>&1)
expect "native variant: single-threaded takes the plain object" "1" "$(echo "$OUT" | tail -1)"
OUT=$("$SPIN" run threaded --allow-native-build 2>&1)
expect "native variant: threaded takes the _mt object" "2" "$(echo "$OUT" | tail -1)"
# --- `spin flags`: the handoff to a build spin does not drive (#4105) ---------
# spin resolves the dependencies and warms the native cache, then prints the
# flags; whoever is driving the build compiles with them. What it prints has to
# be what `spin build` compiles with, so the check is that a hand-driven
# compile produces the same program -- not that the text matches.
cd "$WORK/app"
rm -rf "$XDG_CACHE_HOME/spin/native"          # cold: `flags` must compile the carried C
FLAGS=$("$SPIN" flags 2>/dev/null)
# One line. A cold cache compiles here, and that progress used to print on
# stdout, which would splice `cc fast/fast_ext.c` into the flag string.
[ "$(printf '%s' "$FLAGS" | wc -l | tr -d ' ')" = "0" ] || fail "spin flags: progress leaked onto stdout"
case "$FLAGS" in
  --require-gate*) ;;
  *) fail "spin flags: does not carry the require gate spin build compiles under" ;;
esac
# Absolute throughout: the caller's working directory is its own tree.
case "$FLAGS" in
  *" -I ."*|*" -I ../"*|*" --link ."*|*" --link ../"*)
    fail "spin flags: relative path in [$FLAGS]" ;;
esac
case "$FLAGS" in
  *--link*) ;;
  *) fail "spin flags: carried-C object missing from [$FLAGS]" ;;
esac
SPINEL_BIN=$(dirname "$SPIN")/spinel
"$SPINEL_BIN" $FLAGS bin/app.rb -o "$WORK/handoff" >/dev/null 2>&1 ||
  fail "spin flags: hand-driven compile failed"
expect "spin flags: hand-driven build matches spin build" \
  "$("$SPIN" run 2>&1 | tail -1)" "$("$WORK/handoff" 2>&1 | tail -1)"

# `--deps` names the toolchain files a Makefile must list as prerequisites, or
# its rule answers "up to date" after the compiler changed and the next thing
# run is a binary the old one built (#4386). The runtime archives are in the
# list because a change under lib/ rebuilds them and leaves the compiler
# binary's timestamp alone -- the shape that actually got missed.
DEPS=$("$SPIN" flags --deps 2>&1) || fail "spin flags --deps: exited non-zero"
[ -n "$DEPS" ] || fail "spin flags --deps: printed nothing"
for d in $DEPS; do
  [ -f "$d" ] || fail "spin flags --deps: not a file: $d"
  case "$d" in /*) ;; *) fail "spin flags --deps: relative path: $d" ;; esac
done
case "$DEPS" in
  *spinel*) ;;
  *) fail "spin flags --deps: does not name the compiler [$DEPS]" ;;
esac
case "$DEPS" in
  *libspinel_rt*) ;;
  *) fail "spin flags --deps: does not name a runtime archive [$DEPS]" ;;
esac

# --- carried C is declared, not discovered (#4362) -----------------------------
# `.rb` enters by require-reachability and `.c` used to enter by presence, so an
# application whose repository also holds a C program of its own -- or a scratch
# file an agent left beside the sources -- had its main() compiled into the
# build and collided with the generated one. Nothing is compiled now unless
# [package] sources names it, and what was skipped is reported rather than
# silently dropped.
cd "$WORK"
mkdir -p excl/bin
printf 'puts "excluded ok"\n' > excl/bin/excl.rb
cat > excl/standalone.c <<'EOF'
#include <stdio.h>
int main(void) { puts("a program of my own"); return 0; }
EOF
mkdir -p excl/cbits
printf 'int cbits_unused(void) { return 7; }\n' > excl/cbits/helper.c
cd excl
printf '[package]\nname = "excl"\n' > spin.toml
OUT=$("$SPIN" run 2>&1) || fail "sources: an undeclared main()-bearing .c broke the build"
expect "sources: nothing is carried without a declaration" "excluded ok" "$(echo "$OUT" | tail -1)"
echo "$OUT" | grep -q "standalone.c is not named by \[package\] sources" ||
  fail "sources: the undeclared .c was dropped without saying so"
echo "$OUT" | grep -q "cbits/helper.c is not named by \[package\] sources" ||
  fail "sources: the undeclared .c in a subdirectory was not reported"

# A glob is allowed -- it is the old behaviour, and the author owns it. What it
# picks up beyond the sources is `exclude`'s job: that field now subtracts from
# what `sources` named rather than from everything present.
printf '[package]\nname = "excl"\nsources = ["*.c", "cbits/*.c"]\nexclude = ["standalone.c"]\n' > spin.toml
OUT=$("$SPIN" run 2>&1) || fail "exclude: subtracting from a glob did not build"
expect "exclude: subtracts from what sources named" "excluded ok" "$(echo "$OUT" | tail -1)"
echo "$OUT" | grep -q "^cc excl/cbits/helper.c$" || fail "sources: the declared .c was not compiled"

# Without the subtraction the scratch program is a declared source again, and
# its main() collides with the generated one exactly as it used to.
printf '[package]\nname = "excl"\nsources = ["*.c", "cbits/*.c"]\n' > spin.toml
"$SPIN" build >/dev/null 2>&1 && fail "sources: a declared main()-bearing .c linked in without complaint"

# A declaration that matches nothing is a typo, and the link error it turns into
# names a symbol rather than the manifest. Say it here.
printf '[package]\nname = "excl"\nsources = ["nosuch/*.c"]\n' > spin.toml
"$SPIN" clean >/dev/null 2>&1
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q 'sources entry "nosuch/\*.c" matched no .c file' ||
  fail "sources: a glob matching nothing was not reported"

# --- emitting C over a source spinel did not write is refused (#4362) ---------
# The destructive half of the same problem: `spinel app.rb -c -o sp_json.c`
# inside a package replaces the source with a translation unit, and nothing
# downstream can undo it -- the skip rule below can only say the file is gone.
# Re-emitting over spinel's own output is the ordinary case and stays allowed.
cd "$WORK"
mkdir -p ovw
printf 'puts "ovw ok"\n' > ovw/app.rb
printf 'int mine(void) { return 7; }\n' > ovw/mine.c
cd ovw
"$(dirname "$SPIN")/spinel" app.rb -c -o mine.c >/dev/null 2>&1 && fail "overwrite: emitted over a hand-written source"
expect "overwrite: the source survives" "int mine(void) { return 7; }" "$(cat mine.c)"
"$(dirname "$SPIN")/spinel" app.rb -c -o gen.c >/dev/null 2>&1 || fail "overwrite: a fresh path was refused"
"$(dirname "$SPIN")/spinel" app.rb -c -o gen.c >/dev/null 2>&1 || fail "overwrite: re-emitting over our own output was refused"
"$(dirname "$SPIN")/spinel" app.rb -c -o mine.c --force >/dev/null 2>&1 || fail "overwrite: --force was refused"
case "$(head -1 mine.c)" in
  "/* Generated by Spinel AOT compiler */") : ;;
  *) fail "overwrite: --force did not write" ;;
esac

# --- spinel's own output in the tree is skipped, and said out loud (#4362) -----
# `spinel foo.rb -c -o out.c` inside the package leaves a .c that defines
# main() and, through the internal header, the runtime's non-static surface:
# compiling it buries the build in multiple-definition lines that name symbols
# rather than the file. It is never carried C, so it is left out -- but named
# on stderr, since it may be sitting on top of a source it overwrote.
cd "$WORK"
mkdir -p emitted/bin
printf '[package]\nname = "emitted"\nsources = ["real.c"]\n' > emitted/spin.toml
printf 'puts "emitted ok"\n' > emitted/bin/emitted.rb
cat > emitted/real.c <<'EOF'
#include "spinel/runtime.h"
sp_int sp_emitted_real(void) { return 7; }
EOF
cd emitted
"$(dirname "$SPIN")/spinel" bin/emitted.rb -c -o stray.c >/dev/null 2>&1
# the FIRST build: a second one answers "up to date" without walking the tree,
# so the report would not be reached and the check would pass on nothing
case "$("$SPIN" build 2>&1)" in
  *"emitted/stray.c is spinel's own output"*) : ;;
  *) fail "emitted C: skipped without saying so" ;;
esac
expect "emitted C: the build still runs" "emitted ok" "$("$SPIN" run 2>&1 | tail -1)"

# --- carried C in a subdirectory must not collide with one at the root --------
# The cache named each object after the source's relative path with "/" mapped
# to "_", which is not injective: `a/util.c` and `a_util.c` both asked for
# `a_util.o`, the second compile overwrote the first, and the link then wanted
# a symbol whose object had been replaced. Nothing was said at any point. The
# cache mirrors the tree instead, where a directory cannot collide with a file.
cd "$WORK"
mkdir -p spinel-ocol/a ocolapp/bin
printf '[package]\nname = "ocol"\nsources = ["a_util.c", "a/util.c"]\n' > spinel-ocol/spin.toml
cat > spinel-ocol/ocol.rb <<'EOF'
module Ocol
  native_lib "ocol"
  native_func :flat, [], :int, "sp_ocol_flat_one"
  native_func :nested, [], :int, "sp_ocol_nested_two"
end
EOF
cat > spinel-ocol/a_util.c <<'EOF'
#include "spinel/runtime.h"
sp_int sp_ocol_flat_one(void) { return 1; }
EOF
cat > spinel-ocol/a/util.c <<'EOF'
#include "spinel/runtime.h"
sp_int sp_ocol_nested_two(void) { return 2; }
EOF
printf '[package]\nname = "ocolapp"\n\n[dependencies]\nocol = { path = "../spinel-ocol" }\n' > ocolapp/spin.toml
printf 'require "ocol"\nputs Ocol.flat\nputs Ocol.nested\n' > ocolapp/bin/ocolapp.rb
cd "$WORK/ocolapp"
expect "object cache: a/util.c and a_util.c both survive" "2" "$(SPIN_NO_NATIVE_CACHE=1 "$SPIN" run 2>&1 | tail -1)"

# --- the runtime headers when spin and the compiler are not co-located (#4115) -
# `spin install` puts spin in ~/.local/bin with no spinel beside it, and package
# C includes "spinel/runtime.h". spin gave up locating the headers the moment
# the compiler came from PATH, so -I <spinel>/lib vanished and every package
# carrying C failed on a missing runtime.h -- while the same package built fine
# from a tree where the two sat together.
cd "$WORK"
mkdir -p lonely/bin
cp "$SPIN" lonely/bin/spin                     # spin alone; no spinel beside it
cd "$WORK/app"
OUT=$(PATH="$(dirname "$SPIN"):$PATH" SPIN_NO_NATIVE_CACHE=1 "$WORK/lonely/bin/spin" flags 2>&1) || true
case "$OUT" in
  *"runtime.h"*|*"native compile failed"*)
    fail "headers not found when spin is not beside the compiler: [$OUT]" ;;
esac
case "$OUT" in
  *--link*) ;;
  *) fail "PATH-resolved compiler: carried C did not build [$OUT]" ;;
esac

# ... and through a SYMLINK, which is what `spin install` leaves behind: a link
# in ~/.local/bin pointing into the install tree, with no lib beside the link.
# PATH hands back the link itself, so the walk for the headers started from the
# wrong parent and found nothing (#4126).
#
# The package has to include "spinel/runtime.h" for this to test anything --
# the carried C above includes only <stdint.h>, so it compiles with or without
# the runtime include path, and a check written against it passes either way.
cd "$WORK"
mkdir -p spinel-rthdr linked/bin
printf '[package]\nname = "rthdr"\nsources = ["sp_rthdr.c"]\n' > spinel-rthdr/spin.toml
cat > spinel-rthdr/rthdr.rb <<'EOF'
module Rthdr
  native_lib "rthdr"
  native_func :len2, [:string], :int, "sp_rthdr_len2"
end
EOF
cat > spinel-rthdr/sp_rthdr.c <<'EOF'
#include "spinel/runtime.h"
sp_int sp_rthdr_len2(const char *s) { return (sp_int)sp_str_byte_len(s) * 2; }
EOF
ln -sf "$SPIN" linked/bin/spin
ln -sf "$(dirname "$SPIN")/spinel" linked/bin/spinel
[ -f "$(dirname "$SPIN")/spinel_rbs_extract" ] && ln -sf "$(dirname "$SPIN")/spinel_rbs_extract" linked/bin/spinel_rbs_extract
mkdir -p rtapp/bin
printf '[package]\nname = "rtapp"\n\n[dependencies]\nrthdr = { path = "../spinel-rthdr" }\n' > rtapp/spin.toml
printf 'require "rthdr"\nputs Rthdr.len2("abc")\n' > rtapp/bin/rtapp.rb
cd "$WORK/rtapp"
# SPIN_NO_NATIVE_CACHE, or a cached object is reused and nothing is compiled --
# exactly the masking #4115 complained about, and it made this check pass
# against the bug the first time it was written.
# `|| true`: spin exits non-zero when the compile fails, and under set -e the
# assignment itself would then end the script before the check below could
# say what went wrong -- a reproduction that dies silently.
OUT=$(PATH="$WORK/linked/bin:$PATH" SPIN_NO_NATIVE_CACHE=1 spin flags 2>&1) || true
case "$OUT" in
  *"runtime.h"*|*"native compile failed"*)
    fail "runtime headers not found through a symlinked install: [$OUT]" ;;
esac
expect "symlinked install builds and runs" "6" \
  "$(PATH="$WORK/linked/bin:$PATH" spin run 2>&1 | tail -1)"
cd "$WORK/app"

# --- a native_method returning bytes copies the count, not up to a NUL --------
# A `:cbinstr` return is a borrowed buffer holding raw BYTES, and the callee
# publishes how many in sp_ffi_bin_len. The native_func arm copied that count;
# the native_method arm copied the result as an ordinary C string, so the bytes
# stopped at the first NUL and nothing was reported -- the method answered a
# shorter, valid String. A PNG came back as the 8 bytes of its signature
# (#4821). Both bindings of the same C function are checked here, because the
# two paths answering differently is the whole bug.
cd "$WORK"
rm -rf spinel-binpkg binapp
mkdir -p spinel-binpkg binapp/bin
printf '[package]\nname = "binpkg"\nsources = ["bin.c"]\n' > spinel-binpkg/spin.toml
cat > spinel-binpkg/binpkg.rb <<'EOF'
module BinPackage
  native_struct "Bin", "sp_Bin"
  native_new [], "sp_Bin_new"
  native_method :bytes, [], :cbinstr, "sp_Bin_bytes"
  native_func :bytes, [], :cbinstr, "sp_bin_bytes"
end
EOF
cat > spinel-binpkg/bin.c <<'EOF'
#include "spinel/runtime.h"
typedef struct sp_Bin_s { sp_int cls_id; } sp_Bin;
sp_Bin *sp_Bin_new(sp_int cls_id) { sp_Bin *s = sp_gc_alloc(sizeof(sp_Bin), NULL, NULL); s->cls_id = cls_id; return s; }
static const char bin_bytes[4] = { 'a', 0, 'b', 'c' };
const char *sp_Bin_bytes(sp_Bin *self) { (void)self; sp_ffi_bin_len = 4; return bin_bytes; }
const char *sp_bin_bytes(void) { sp_ffi_bin_len = 4; return bin_bytes; }
EOF
printf '[package]\nname = "binapp"\n\n[dependencies]\nbinpkg = { path = "../spinel-binpkg" }\n' > binapp/spin.toml
printf 'require "binpkg"\nputs Bin.new.bytes.bytesize\nputs BinPackage.bytes.bytesize\n' > binapp/bin/binapp.rb
cd "$WORK/binapp"
expect "native_method :cbinstr keeps the bytes past a NUL" "4 4" \
  "$("$SPIN" run 2>&1 | tail -2 | tr '\n' ' ' | sed 's/ $//')"
cd "$WORK/app"

# --- the native cache is relocatable and skippable (#4115) --------------------
# A run behaves differently depending on whether an object is already cached,
# which is what you least want while working out why a build differs.
rm -rf "$WORK/ncache"
OUT=$(SPIN_NATIVE_CACHE="$WORK/ncache" "$SPIN" flags 2>/dev/null)
case "$OUT" in
  *"$WORK/ncache"*) ;;
  *) fail "SPIN_NATIVE_CACHE ignored: [$OUT]" ;;
esac
# Cached: the second run compiles nothing. Not cached: it compiles again.
CC1=$(SPIN_NATIVE_CACHE="$WORK/ncache" "$SPIN" flags 2>&1 >/dev/null | grep -c "^cc " || true)
expect "native cache reused on the second run" "0" "$CC1"
CC2=$(SPIN_NATIVE_CACHE="$WORK/ncache" SPIN_NO_NATIVE_CACHE=1 "$SPIN" flags 2>&1 >/dev/null | grep -c "^cc " || true)
[ "$CC2" -ge 1 ] || fail "SPIN_NO_NATIVE_CACHE did not force a rebuild"

# --- the build-failed hint belongs to one failure, not to all of them (#4136) -
# `spin add <name>` fixes an unresolved require and nothing else, so pointing
# at it after a parse error is the last line the reader sees pointing away from
# the fix.
cd "$WORK"
mkdir -p hintprj/bin
cd hintprj
"$SPIN" init >/dev/null 2>&1
printf 'def f(\nputs 1\n' > bin/hintprj.rb
OUT=$("$SPIN" build 2>&1) || true
case "$OUT" in
  *"spin add"*) fail "parse error got the unresolved-require hint: [$OUT]" ;;
esac
case "$OUT" in
  *"build failed"*) ;;
  *) fail "parse error did not report a build failure: [$OUT]" ;;
esac
printf 'require "nosuchlib_e2e"\nputs 1\n' > bin/hintprj.rb
OUT=$("$SPIN" build 2>&1) || true
case "$OUT" in
  *"spin add"*) ;;
  *) fail "unresolved require lost its hint: [$OUT]" ;;
esac
# The compiler's own diagnostics still reach the user either way.
case "$OUT" in
  *"cannot load such file"*) ;;
  *) fail "compiler stderr not replayed: [$OUT]" ;;
esac
printf 'puts 42\n' > bin/hintprj.rb
expect "a good build still runs" "42" "$("$SPIN" run 2>&1 | tail -1)"

# --- spin ext: scaffold, build, differential (skips without ruby.h) -----------
cd "$WORK"
if command -v ruby >/dev/null 2>&1 && [ -f "$(ruby -e 'puts RbConfig::CONFIG["rubyhdrdir"]' 2>/dev/null)/ruby.h" ]; then
  "$SPIN" ext new fastx >/dev/null || fail "ext new"
  cd fastx
  expect "ext kernel runs as plain Ruby" "42" "$(ruby lib/fastx/kernel.rb 2>&1 | tail -1)"
  "$SPIN" ext build >/dev/null 2>&1 || fail "ext build"
  [ -f ext/fastx/fastx.c ] || fail "ext build: no kernel C"
  [ -f ext/fastx/fastx.h ] || fail "ext build: no header contract"
  [ -f ext/fastx/fastx_ext.c ] || fail "ext build: no shim"
  [ -f ext/fastx/sp_gc.c ] || fail "ext build: runtime not vendored"
  OUT=$("$SPIN" ext test 2>&1 | tail -1)
  case "$OUT" in
    *"3/3 match"*) ;;
    *) fail "ext test differential: [$OUT]" ;;
  esac
  # the differential compares against the compiled extension: a kernel edited
  # after the build diverges from it
  cp lib/fastx/kernel.rb kernel.rb.orig
  sed 's/n \* 2/n * 3/' kernel.rb.orig > lib/fastx/kernel.rb
  OUT=$("$SPIN" ext test 2>&1) || true
  case "$OUT" in
    *"2 case(s) diverge"*) ;;
    *) fail "ext test did not load the compiled extension: [$OUT]" ;;
  esac
  mv kernel.rb.orig lib/fastx/kernel.rb
  expect "ext loader finds the built extension" "24 1" \
    "$(ruby -I build/ext -I lib -e 'require "fastx"; puts "#{Fastx.double(12)} #{$LOADED_FEATURES.grep(/fastx\/fastx\./).size}"' 2>&1 | tail -1)"
  # spin found through PATH (argv[0] is the bare name) vendors the runtime too
  cd "$WORK"
  ( PATH="$(dirname "$SPIN"):$PATH"; spin ext new pathx >/dev/null && cd pathx && spin ext build >/dev/null 2>&1 ) ||
    fail "ext build through PATH"
  [ -f pathx/ext/pathx/spinel_rt.h ] || fail "ext build through PATH: runtime not vendored"
  # a header-only SPINEL_HDR_DIR has no runtime sources to vendor: refused
  mkdir -p hdronly && cp pathx/ext/pathx/spinel_rt.h hdronly/
  ( cd pathx && SPINEL_HDR_DIR="$WORK/hdronly" "$SPIN" ext build >/dev/null 2>&1 ) &&
    fail "ext build vendored from a header-only SPINEL_HDR_DIR"
  cd fastx
  # the loader falls back to the plain kernel when no extension is built
  expect "ext fallback require" "24" "$(ruby -I lib -e 'require "fastx"; puts Fastx.double(12)' 2>&1 | tail -1)"
else
  echo "spin-e2e: ext case skipped (no ruby.h)"
fi

# --- spin pack: a directory that builds from C alone --------------------------
# The claim `spin pack` makes is that the recipient needs a C compiler and make
# and nothing else, so the check is to BUILD it that way and compare answers --
# not to look at the Makefile. The program uses threads and a bundled native
# package on purpose: those are what make the pack more than a copy, since the
# threaded runtime needs -DSP_THREADS to reach the runtime sources as well as
# the generated TU, and a package travels as source rather than as the object
# built for the packer's platform. The dependency with carried C (spinel-fast,
# from the M2 case above) is the other kind of native package: a bundled one
# compiles beside its source, a dependency compiles into the shared cache, and
# the pack has to find the source of each.
# A dependency whose C is laid out like a vendored library travels in its own
# tree: a source in a subdirectory that includes a header and an `.inc` table
# from the package root (`spin build` compiles with -I at the root), and a
# source whose basename another dependency also uses -- flattened into one
# native/ directory, the two `fast_ext.c` were one file.
cd "$WORK"
mkdir -p spinel-deep/src spinel-deep/inc
printf '[package]\nname = "deep"\nsources = ["src/*.c"]\n' > spinel-deep/spin.toml
printf 'module Deep\n  ffi_func :deep_triple, [:int], :int\nend\n' > spinel-deep/deep.rb
printf '#define DEEP_FACTOR 3\n' > spinel-deep/inc/deep.h
printf 'DEEP_FACTOR\n' > spinel-deep/inc/factor.inc
cat > spinel-deep/src/fast_ext.c <<'CEOF'
#include <stdint.h>
#include "inc/deep.h"
intptr_t deep_triple(intptr_t x) { return x * (
#include "inc/factor.inc"
); }
CEOF
"$SPIN" new packer >/dev/null || fail "pack: new"
cd packer
printf '[package]\nname = "packer"\n\n[dependencies]\nfast = { path = "../spinel-fast" }\ndeep = { path = "../spinel-deep" }\n' > spin.toml
cat > bin/packer.rb <<'RBEOF'
require "json"
require "fast"
require "deep"
ths = (0...4).map { |i| Thread.new(i) { |n| n * 10 } }
puts "threads #{ths.map(&:value).inspect}"
puts "json #{JSON.generate({ "a" => 1, "b" => [2, 3] })} fast #{Fast.fast_quad(10)} deep #{Deep.deep_triple(10)} crypt #{"spin".crypt("ab")}"
RBEOF
REF=$("$SPIN" run 2>&1 | tail -2 | tr '\n' '|')
"$SPIN" pack >/dev/null 2>&1 || fail "pack: spin pack"
[ -f build/pack/packer/Makefile ] || fail "pack: no Makefile"
[ -f build/pack/packer/src/packer.c ] || fail "pack: no generated C"
[ -f build/pack/packer/lib/sp_gc.c ] || fail "pack: no runtime source"
[ -f build/pack/packer/lib/spinel/runtime.h ] || fail "pack: no package ABI header"
[ -f build/pack/packer/native/sp_json.c ] || fail "pack: native package not carried as source"
[ -f build/pack/packer/native/fast/fast_ext.c ] || fail "pack: dependency's native C not carried as source"
[ -f build/pack/packer/native/deep/src/fast_ext.c ] || fail "pack: a dependency's tree not mirrored"
[ -f build/pack/packer/native/deep/inc/factor.inc ] || fail "pack: a dependency's .inc not carried"
grep -q -- "-DSP_THREADS" build/pack/packer/Makefile || fail "pack: threaded program without -DSP_THREADS"
grep -q -- "-lpthread" build/pack/packer/Makefile || fail "pack: threaded program without -lpthread"
# -lcrypt is the recipient's platform's to decide (glibc ships it, Darwin does
# not), so it must not be baked from the packer's: the Makefile carries the
# uname conditional, and the program's String#crypt above links through it.
grep -q -- "^LIBS ?=.*-lcrypt" build/pack/packer/Makefile && fail "pack: -lcrypt baked from the packer's platform"
grep -q -- "uname -s" build/pack/packer/Makefile || fail "pack: Makefile does not decide -lcrypt by platform"
# What the pack must NOT carry: how THIS build was run. The compiler reports
# ingredients, not a command line, so the packer's optimisation level, its
# warning policy, its diagnostic formatting and its linker's spelling of
# section GC stay behind. These reached the recipient's Makefile when the pack
# was derived from `--print-cc`, and an unfamiliar compiler rejects them.
for host_flag in -Werror= -fmax-errors -fno-show-column -fno-diagnostics-show-caret -fno-caret-diagnostics --gc-sections -dead_strip -ffunction-sections; do
  grep -q -- "$host_flag" build/pack/packer/Makefile && \
    fail "pack: Makefile carries the packer's own $host_flag"
done
# the build itself, with PATH holding no spinel and no spin
( cd build/pack/packer && make -j4 >/dev/null 2>&1 ) || fail "pack: make in the pack"
OUT=$( cd build/pack/packer && ./packer 2>&1 | tail -2 | tr '\n' '|' )
expect "pack builds from C alone and answers the same" "$REF" "$OUT"

# The recipient's compiler is theirs to choose -- the reason the report names
# no cc at all. Prove the Makefile actually routes through $(CC) rather than
# naming one: build again through a wrapper and check the wrapper ran. A
# wrapper is the portable version of this test; asserting on `clang` would
# only run where clang happens to be installed.
cat > "$WORK/wrapcc" <<WRAPEOF
#!/bin/sh
echo used >> "$WORK/wrapcc.log"
exec ${CC:-cc} "\$@"
WRAPEOF
chmod +x "$WORK/wrapcc"
rm -f "$WORK/wrapcc.log"
( cd build/pack/packer && make clean >/dev/null 2>&1 && make -j4 CC="$WORK/wrapcc" >/dev/null 2>&1 ) || \
  fail "pack: make CC=<other compiler> in the pack"
[ -s "$WORK/wrapcc.log" ] || fail "pack: Makefile ignored CC"
OUT2=$( cd build/pack/packer && ./packer 2>&1 | tail -2 | tr '\n' '|' )
expect "pack honours CC and answers the same" "$REF" "$OUT2"

# A cross build for a target without pthread. The host has it; the recipient's
# $(CC) may not, and the packer cannot know. This "device" toolchain has no
# <pthread.h> and refuses -pthread/-lpthread. The threaded pack above must stop
# at make's first line with the reason, not forty files in on the header;
# a pack of a program without threads must build and run with the same cc,
# since the single-threaded runtime needs no pthread at all.
mkdir -p "$WORK/nopt/inc"
printf '#error "no pthread on this target"\n' > "$WORK/nopt/inc/pthread.h"
cat > "$WORK/nopt/cc" <<NOPTEOF
#!/bin/sh
for a in "\$@"; do case "\$a" in -lpthread|-pthread) echo "nopthread-cc: unknown option \$a" >&2; exit 1;; esac; done
exec ${CC:-cc} -I"$WORK/nopt/inc" "\$@"
NOPTEOF
chmod +x "$WORK/nopt/cc"
NOPT_OUT=$( cd build/pack/packer && make clean >/dev/null 2>&1; make -j4 CC="$WORK/nopt/cc" 2>&1 || true )
case "$NOPT_OUT" in
  *"uses Thread"*"has no pthread"*) ;;
  *) fail "pack: a threaded pack on a target without pthread did not name the reason: $NOPT_OUT" ;;
esac
cd "$WORK"
"$SPIN" new plainpack >/dev/null || fail "pack: new plainpack"
cd plainpack
printf 'puts "plain #{[1, 2, 3].sum}"\n' > bin/plainpack.rb
"$SPIN" pack >/dev/null 2>&1 || fail "pack: spin pack plainpack"
grep -q "SP_PTHREAD" build/pack/plainpack/Makefile && fail "pack: an unthreaded pack carries the pthread probe"
( cd build/pack/plainpack && make -j4 CC="$WORK/nopt/cc" >/dev/null 2>&1 ) || \
  fail "pack: an unthreaded pack did not build on a target without pthread"
expect "an unthreaded pack runs on a target without pthread" "plain 6" "$( cd build/pack/plainpack && ./plainpack 2>&1 )"

echo "spin-e2e: ALL GREEN"
