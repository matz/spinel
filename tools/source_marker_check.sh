#!/bin/sh
# #<SPINEL_SOURCE>file:line (#7630): the lines after the marker are reported at
# that position, and what the program does does not change.
SPINEL=${SPINEL:-bin/spinel}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-srcmark.XXXXXX") || exit 2
tmp=$(cd "$tmp" && pwd -P) || exit 2   # no "//" from a TMPDIR ending in "/": a required file's path is written without it
trap 'rm -rf "$tmp"' EXIT
ok=1
fail() { echo "source-marker-test: FAIL ($1)"; ok=0; }

cat > "$tmp/greeting.rb" <<'RB'
# generated from greeting.html.erb by a transpiler
def greeting(io, name)
#<SPINEL_SOURCE>greeting.html.erb:1
  io << "<p>Hello, "
  io << name.upcase
  io << "!</p>\n"
#<SPINEL_SOURCE>greeting.html.erb:2
  io << "<p>Seen from "
  io << File.basename(__dir__)
  io << "</p>\n"
  nil
end
RB
cat > "$tmp/main.rb" <<'RB'
require_relative "greeting"
io = String.new
greeting(io, "world")
print io
puts __LINE__
#<SPINEL_SOURCE>weird name.erb:7:3
puts __FILE__ == $0
#<SPINEL_SOURCE>nocolon
#<SPINEL_SOURCE>:5
#<SPINEL_SOURCE>x.erb:0
puts 1
RB
cd "$tmp" || exit 2
"$OLDPWD/$SPINEL" main.rb -o app 2>err.txt || { cat err.txt; fail "compile"; }
./app > got.txt
printf '<p>Hello, WORLD!</p>\n<p>Seen from %s</p>\n5\ntrue\n1\n' "$(basename "$tmp")" > want.txt
cmp -s got.txt want.txt || { diff want.txt got.txt; fail "the program's output changed (__dir__/__FILE__/__LINE__ stay physical)"; }
"$OLDPWD/$SPINEL" main.rb -S > out.c 2>/dev/null
grep -q '^#line 1 "greeting.html.erb"' out.c || fail "no #line 1 greeting.html.erb"
grep -q '^#line 2 "greeting.html.erb"' out.c || fail "no #line 2 greeting.html.erb"
grep -q '^#line 7 "weird name.erb:7"\|^#line 3 "weird name.erb:7"' out.c || fail "a path with a colon splits at the last one"
grep -q '^#line 2 "greeting.rb"' out.c || fail "lines before the first marker keep their place"
grep -q 'nocolon\|"x.erb"\|^#line [0-9]* ""' out.c && fail "a malformed marker was taken"

# Which file's `# spinel: defer-refusals` governs a line is the physical file's
# own, whatever name a marker gives it: a marker in a deferring file keeps the
# deferral, a marker in a file without the comment never takes one from the
# deferring file whose path it names, and two files that give one name each
# keep their own answer, in either order they are read.
sp="$OLDPWD/$SPINEL"
refused='  "a".unicode_normalize(:nfc)'
# a refusal in several kinds of statement, so the nodes a rewrite makes over them are asked too
many() { cat <<RB
def plain = "a".unicode_normalize(:nfc)
def forwarded(...) = plain(...)
def blocked = [1].map { |i| "a#{i}".unicode_normalize(:nfc) }
def interpolated = "v=#{"a".unicode_normalize(:nfc)}"
def ensured
  yield
ensure
  "a".unicode_normalize(:nfc)
end
[:plain, :forwarded, :blocked, :interpolated].each { |m| begin; send(m); rescue NotImplementedError; puts m; end }
begin; ensured { 1 }; rescue NotImplementedError; puts :ensured; end
RB
}
{ echo "# spinel: defer-refusals"; echo "#<SPINEL_SOURCE>t.erb:3"; many; echo "puts 1"; } > pm.rb
{ echo "# spinel: defer-refusals"; many; echo "puts 1"; } > pn.rb
"$sp" pm.rb -o pm > pm.out 2>&1 || { cat pm.out; fail "a deferring file with a marker was refused"; }
"$sp" pn.rb -o pn > pn.out 2>&1 || { cat pn.out; fail "a deferring file was refused"; }
[ "$(grep -c 'refusals deferred to run time' pm.out)" = 1 ] && [ "$(sed -n 's/.*spinel: \([0-9]*\) refusals deferred.*/\1/p' pm.out)" = "$(sed -n 's/.*spinel: \([0-9]*\) refusals deferred.*/\1/p' pn.out)" ] ||
  fail "a marker changed how many refusals a deferring file defers"
./pm > pm.got; ./pn > pn.got
[ "$(tr '\n' ' ' < pm.got)" = "plain forwarded blocked interpolated ensured 1 " ] && cmp -s pm.got pn.got || fail "a deferring file with a marker ran wrong"
{ echo "#<SPINEL_SOURCE>t.erb:3"; many; echo "puts 1"; } > mn.rb
"$sp" pm.rb -S > pm.c 2>/dev/null
grep -q '^#line 3 "t.erb"' pm.c || fail "a deferring file's lines under a marker are not shown at the marker's position"
"$sp" mn.rb -o mn > mn.out 2>&1 && fail "a marker made a file without the comment defer"
grep -q '^spinel: t.erb:' mn.out || fail "a refusal is not shown at the marker's position"

cat > dep.rb <<'RB'
# spinel: defer-refusals
def dep = 1
RB
{ echo 'require_relative "dep"'; echo "#<SPINEL_SOURCE>$tmp/dep.rb:1"; echo 'def normalize(s)'; echo "$refused"; echo end; echo 'puts dep'; echo 'begin; normalize("x"); rescue NotImplementedError; puts :r; end'; } > named.rb
# (by its absolute path: a require_relative then names its file as the marker does)
"$sp" "$tmp/named.rb" -o named > named.out 2>&1 && fail "a marker naming a deferring file made a file without the comment defer"
grep -q "^spinel: $tmp/dep.rb:1:" named.out || fail "a refusal under a marker naming a file is not shown at that file"

for order in "a b" "b a"; do
  cat > a.rb <<'RB'
# spinel: defer-refusals
#<SPINEL_SOURCE>shared.erb:1
def from_a = "a".unicode_normalize(:nfc)
RB
  printf '#<SPINEL_SOURCE>shared.erb:1\ndef from_b = 1\n' > b.rb
  : > req.rb; for f in $order; do echo "require_relative \"$f\"" >> req.rb; done
  { cat req.rb; echo 'begin; from_a; rescue NotImplementedError; puts :a_deferred; end'; echo 'puts from_b'; } > two.rb
  "$sp" two.rb -o two > two.out 2>&1 || { cat two.out; fail "two files sharing a marker name: the deferring one was refused ($order)"; }
  [ "$(./two | tr '\n' ' ')" = "a_deferred 1 " ] || fail "two files sharing a marker name ran wrong ($order)"
  "$sp" two.rb -S > two.c 2>/dev/null
  [ "$(grep -c '^#line 1 "shared.erb"' two.c)" -ge 1 ] || fail "two files sharing a marker name: the position is not shown ($order)"
  printf '#<SPINEL_SOURCE>shared.erb:1\ndef from_b = "b".unicode_normalize(:nfc)\n' > b.rb
  "$sp" two.rb -o two > two.out 2>&1 && fail "two files sharing a marker name: the one without the comment deferred ($order)"
  grep -q '^spinel: shared.erb:' two.out || fail "two files sharing a marker name: no refusal shown at the marker ($order)"
done
# a marker naming the entry script (file 0, which also means "no marker" to the
# line map) is not taken: the lines stay at the file they are in
cat > lib0.rb <<'RB'
# spinel: defer-refusals
RB
printf '#<SPINEL_SOURCE>%s/entry.rb:40\ndef from_lib = "a".unicode_normalize(:nfc)\n' "$tmp" >> lib0.rb
printf 'require_relative "lib0"\nbegin; from_lib; rescue NotImplementedError => e; puts e.message[/\\A[^:]*lib0.rb:\\d+/]; end\n' > "$tmp/entry.rb"
"$sp" "$tmp/entry.rb" -o entry > entry.out 2>&1 || { cat entry.out; fail "a marker naming the entry script made a deferring file refused"; }
[ "$(./entry)" = "$tmp/lib0.rb:3" ] || fail "a marker naming the entry script moved a deferring file's position"
[ $ok -eq 1 ] && echo "source-marker-test: pass" || exit 1
