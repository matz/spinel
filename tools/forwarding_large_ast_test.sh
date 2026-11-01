#!/bin/sh
# The `...` forwarding rewrite keeps a workspace as long as the program. A
# program-sized array on the C stack overflows a small stack on a large input
# that is only flat: the prefix below has no nesting, and the stack limit
# stands in for a larger program. Run from the Spinel root.
set -eu
SPINEL=${SPINEL:-bin/spinel}
src=test/forwarding_large_ast.rb
mkdir -p build
tmp=$(mktemp -d build/forwarding-large-ast.XXXXXX)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

awk 'BEGIN { for (i = 0; i < 150000; i++) print "nil" }' > "$tmp/large.rb"
cat "$src" >> "$tmp/large.rb"

# The fixture alone compiles under the small stack, so the flat prefix is
# what the next compile adds.
if ! (ulimit -s 512 && exec "$SPINEL" "$src" -c -o "$tmp/small.c") > "$tmp/small.log" 2>&1; then
  cat "$tmp/small.log"
  echo "forwarding-large-ast-test: FAIL (the fixture alone did not compile with a 512 KiB stack)"
  exit 1
fi
if ! (ulimit -s 512 && exec "$SPINEL" "$tmp/large.rb" -c -o "$tmp/large.c") > "$tmp/large.log" 2>&1; then
  cat "$tmp/large.log"
  echo "forwarding-large-ast-test: FAIL (the large flat input did not compile with a 512 KiB stack)"
  exit 1
fi
[ -s "$tmp/large.c" ] || { echo "forwarding-large-ast-test: FAIL (no C written)"; exit 1; }
echo "forwarding-large-ast-test: pass"
