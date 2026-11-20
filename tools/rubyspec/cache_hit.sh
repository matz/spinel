#!/bin/sh
# The driver's SPINEL_SKIP_BUILD_IF predicate. Save the key for the worker
# that runs a miss; only a complete cached PASS can stand in for that run.
# Cache I/O errors are misses. Keep them out of the compiler diagnostic
# that the worker uses to classify a native compile failure.
exec 2>/dev/null
key=$("$RC" key "$1" "$2" "$RC_FP") && [ -n "$key" ] || exit 1
printf '%s\n' "$key" > "$RUBYSPEC_CACHE_SLOT.key" || exit 1
hit=$("$RC" get "$key") || exit 1
case "$hit" in
  PASS|PASS"	"*) printf '%s\n' "$hit" > "$RUBYSPEC_CACHE_SLOT.hit";;
  *) exit 1;;
esac
