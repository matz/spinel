#!/bin/sh
# Symbol literals use PolyArray, so build the exported Symbol Array ABI too.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/../test/concat-repr" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
CC=${CC:-cc}
SPINEL=${SPINEL:-bin/spinel}
REF_RUBY=${REF_RUBY:-ruby}
TIMEOUT=${SPINEL_TIMEOUT:-build/spinel-timeout}

$REF_RUBY --enable-frozen-string-literal "$src/symbol_bridge.rb" > "$tmp/expected"
diff -u "$src/symbol_bridge.rb.expected" "$tmp/expected"
$CC ${CFLAGS:-} ${SEC_FLAGS:-} -DSP_ONE_STRING -Ilib -c "$src/symbol_bridge.c" -o "$tmp/bridge.o"
for opt in 0 2; do
  "$SPINEL" "$src/symbol_bridge.rb" --cc="$CC" -O"$opt" --repr-check --plan-check \
    --link "$tmp/bridge.o" -o "$tmp/symbol" > "$tmp/compile.log" 2>&1 || {
      cat "$tmp/compile.log"
      exit 1
    }
  for verify in 0 1; do
    if SPINEL_GC_STRESS=2 SPINEL_GC_VERIFY=$verify "$TIMEOUT" 60 "$tmp/symbol" \
        > "$tmp/actual" 2> "$tmp/stderr"; then
      diff -u "$tmp/expected" "$tmp/actual"
    else
      cat "$tmp/stderr"
      echo "concat-repr-test: FAIL (O$opt, stress 2, verifier $verify)"
      exit 1
    fi
    echo "concat-repr-test: pass (O$opt, stress 2, verifier $verify)"
  done
done
