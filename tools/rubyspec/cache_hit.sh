#!/bin/sh
# The driver's SPINEL_SKIP_BUILD_IF predicate. Save the key for the worker
# that runs a miss; only a complete cached PASS can stand in for that run.
# Cache I/O errors are misses. Keep them out of the compiler diagnostic
# that the worker uses to classify a native compile failure.
exec "$RC" lookup "$RUBYSPEC_CACHE_SLOT.key" "$RUBYSPEC_CACHE_SLOT.hit" "$1" "$2" "$RC_FP" 2>/dev/null
