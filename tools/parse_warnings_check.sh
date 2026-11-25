#!/bin/sh
# The parser's warnings are written when a program compiles (#12532), as
# CRuby writes them when it reads the program: the ones of the default level
# always (a duplicated Hash key, `= literal` in a conditional), the ones
# ruby -w adds (an unused local, a statement not reached) under -w only. Each
# names the file of the program it is in, a required file by its own path; a
# library of the compiler's writes none. The entry's come first, as CRuby
# reads the entry before its requires run. The text of -e is named -e, and
# its top-level locals are not taken for unused, as under ruby -e.
SPINEL=${SPINEL:-bin/spinel}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-parsewarn.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
ok=1
fail() { echo "parse-warnings-test: FAIL ($1)"; ok=0; }
dir=test/parse_warnings

warnings() { grep 'warning:' "$1"; }

"$SPINEL" "$dir/main.rb" -c -o "$tmp/m.c" 2>"$tmp/d.err" || fail "main.rb did not compile"
warnings "$tmp/d.err" > "$tmp/d.got"
cat > "$tmp/d.want" <<WANT
spinel: $dir/main.rb:4: warning: key :team_id is duplicated and overwritten on line 4
spinel: $dir/main.rb:7: warning: found '= literal' in conditional, should be ==
spinel: $dir/_lib.rb:4: warning: key :k is duplicated and overwritten on line 4
WANT
cmp -s "$tmp/d.got" "$tmp/d.want" || { fail "default warnings"; diff "$tmp/d.want" "$tmp/d.got"; }

"$SPINEL" -w "$dir/main.rb" -c -o "$tmp/m.c" 2>"$tmp/w.err" || fail "main.rb did not compile under -w"
warnings "$tmp/w.err" > "$tmp/w.got"
cat > "$tmp/w.want" <<WANT
spinel: $dir/main.rb:4: warning: key :team_id is duplicated and overwritten on line 4
spinel: $dir/main.rb:7: warning: found '= literal' in conditional, should be ==
spinel: $dir/main.rb:12: warning: statement not reached
spinel: $dir/main.rb:10: warning: assigned but unused variable - y
spinel: $dir/_lib.rb:4: warning: key :k is duplicated and overwritten on line 4
spinel: $dir/_lib.rb:3: warning: assigned but unused variable - unused
WANT
cmp -s "$tmp/w.got" "$tmp/w.want" || { fail "-w warnings"; diff "$tmp/w.want" "$tmp/w.got"; }

"$SPINEL" -w -e 'z = 1; h = {a: 1, a: 2}; p h' -c -o "$tmp/e.c" 2>"$tmp/e.err" || fail "-e did not compile"
warnings "$tmp/e.err" > "$tmp/e.got"
echo "spinel: -e:1: warning: key :a is duplicated and overwritten on line 1" > "$tmp/e.want"
cmp -s "$tmp/e.got" "$tmp/e.want" || { fail "-e warnings"; diff "$tmp/e.want" "$tmp/e.got"; }

[ $ok = 1 ] && echo "parse-warnings-test: OK"
[ $ok = 1 ]
