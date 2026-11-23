#!/bin/sh
# A local variable node, a parameter and a block-local name the scope that
# declares them: its `decl` is that scope's node (a program, class, module,
# singleton class, def, block or lambda), where Prism's `depth` only counted
# scopes outward. The parse step writes each name as the program wrote it;
# comp_name_locals gives it its slot later. Each test/decl/NAME.rb has a
# NAME.decl listing its local nodes as `Kind name @line -> Scope @line`, the
# scope each is declared in; `tools/decl_check.sh --write` regenerates the
# listings.
SPINEL=${SPINEL:-bin/spinel}
write=0; [ "$1" = --write ] && write=1
ok=1
for t in test/decl/*.rb; do
  dump=$($SPINEL --dump-ast "$t" 2>&1) || { echo "decl-test: FAIL ($t): $SPINEL --dump-ast failed"; ok=0; continue; }
  got=$(printf '%s\n' "$dump" | awk '
    $1 == "N" { type[$2] = $3 }
    $1 == "S" && $3 == "name" { name[$2] = $4 }
    $1 == "I" && $3 == "node_line" { line[$2] = $4 }
    $1 == "I" && $3 == "decl" { decl[$2] = $4; order[++n] = $2 }
    END { for (i = 1; i <= n; i++) { id = order[i]; d = decl[id]
            printf "%s %s @%s -> %s @%s", type[id], name[id], line[id], type[d], line[d]
            printf "\n" } }')
  if [ $write = 1 ]; then printf '%s\n' "$got" > "${t%.rb}.decl"; continue; fi
  if [ "$got" != "$(cat "${t%.rb}.decl")" ]; then
    echo "decl-test: FAIL ($t)"; printf '%s\n' "$got" | diff -u "${t%.rb}.decl" - | head -10; ok=0
  fi
done
[ $ok = 1 ] && echo "decl-test: pass" || exit 1
