#!/bin/sh
# An oracle for `make bisect-test`: the build is wrong when every key in
# $CULPRITS (space separated) is allowed, and cannot be judged when $BREAKS
# is allowed without $WITH. It reads only the allow-list, as an oracle that
# starts its own compile would inherit it.
[ -z "$SPINEL_DECISIONS" ] && exit 1
allowed() { grep -qxF "$1" "$SPINEL_DECISIONS"; }
if [ -n "$BREAKS" ] && allowed "$BREAKS" && ! allowed "$WITH"; then exit 125; fi
for k in $CULPRITS; do allowed "$k" || exit 0; done
exit 1
