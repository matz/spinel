#!/bin/sh
# A stand-in for the compiler in `make bisect-test`: every program it
# "compiles" takes two keyed decisions, and the binary it writes prints which
# way the first one went, under a line that is wrong either way. With
# FAKE_BREAK set, the first does not build unless the second is taken too;
# with FAKE_UNSTEADY, the binary also prints its pid.
out=""; conly=0
while [ $# -gt 0 ]; do
  case "$1" in -o) out=$2; shift ;; -c) conly=1 ;; esac
  shift
done
narrow='nn-read@fake.rb:2:5:x'; save='gc-save@#helper'
[ -n "$SPINEL_DECISIONS_LOG" ] && printf '%s\n%s\n' "$narrow" "$save" > "$SPINEL_DECISIONS_LOG"
taken() { [ -z "$SPINEL_DECISIONS" ] || grep -qxF "$1" "$SPINEL_DECISIONS"; }
if [ -n "$FAKE_BREAK" ] && taken "$narrow" && ! taken "$save"; then
  echo "fake_spinel: does not build" >&2; exit 1
fi
if [ $conly = 1 ]; then : > "$out"; exit 0; fi
if taken "$narrow"; then answer=narrowed; else answer=guarded; fi
printf '#!/bin/sh\necho "an older bug"\necho %s\n' "$answer" > "$out"
[ -n "$FAKE_UNSTEADY" ] && echo 'echo $$' >> "$out"
chmod +x "$out"
