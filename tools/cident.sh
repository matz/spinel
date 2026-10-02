#!/usr/bin/env bash
# cident.sh -- prove a change leaves every generated C file byte-identical.
#
#   tools/cident.sh <ref-rev>
#
# Compiles every program of the corpus (test/, benchmark/, packages/*/test/
# and optcarrot) to C with -c --no-line-map, once with the compiler of
# <ref-rev> and once with this tree's bin/spinel, and compares the files
# byte for byte. The reference tree is built in a worktree and its C is
# cached under build/cident/<sha>/, so comparing many commits against one
# base costs one build. Programs one side refuses and the other compiles
# are reported too: the set of refused programs must not change either.
#
# A refactor of the analysis -> codegen boundary (#7100) must report
# 0 differing and 0 refusal changes at every commit. Renumbered temps
# (_tN) count as a difference: they mean the emission order changed.
#
# Exit status: 0 identical, 1 some file differs, 2 infrastructure error.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
REV=${1-}
[ -n "$REV" ] || { echo "usage: $0 <ref-rev>" >&2; exit 2; }
SHA=$(git rev-parse --verify -q "$REV^{commit}") || { echo "cident: unknown revision $REV" >&2; exit 2; }
JOBS=${CIDENT_JOBS:-$(nproc)}
NEW=$ROOT/bin/spinel
[ -x "$NEW" ] || { echo "cident: build bin/spinel first" >&2; exit 2; }

# The corpus, as paths relative to the tree root.
OC=build/optcarrot-single.rb
list() {
  ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  [ -f "$OC" ] && echo "$OC"
}

# emit <spinel> <tree-root> <outdir>: C for every corpus file, or a .refused
# marker where the compiler refuses it. Paths stay relative so that the
# compiler's own tree resolves lib/ and packages/.
emit() {
  local sp=$1 tree=$2 out=$3
  mkdir -p "$out"
  list | xargs -P "$JOBS" -I{} sh -c '
    f="$1"; sp="$2"; tree="$3"; out="$4"
    key=$(printf "%s" "$f" | tr "/" "_")
    if (cd "$tree" && "$sp" -c --no-line-map "$f" -o "$out/$key.c" >/dev/null 2>&1); then :
    else rm -f "$out/$key.c"; : > "$out/$key.refused"; fi
  ' _ {} "$sp" "$tree" "$out"
}

REFDIR=$ROOT/build/cident/$SHA
if [ ! -f "$REFDIR/.done" ]; then
  # The C embeds the compiler's own tree path in a few string literals,
  # together with their lengths. A reference tree at a path of the same
  # length as this one lets a plain rename make those bytes equal.
  L=${#ROOT}
  WT=$(printf "/tmp/cident-%s%0100d" "$SHA" 0 | cut -c1-"$L")
  [ "$L" -ge 20 ] && [ ${#WT} -eq "$L" ] || { echo "cident: cannot place a reference tree beside $ROOT" >&2; exit 2; }
  git worktree remove --force "$WT" >/dev/null 2>&1; rm -rf "$WT"
  git worktree add --detach "$WT" "$SHA" >/dev/null 2>&1 || { echo "cident: cannot check out $REV" >&2; exit 2; }
  [ -d "$ROOT/vendor" ] && [ ! -d "$WT/vendor" ] && cp -r "$ROOT/vendor" "$WT/vendor"
  ( cd "$WT" && make -j"$JOBS" -s >/dev/null 2>&1 ) || {
    echo "cident: reference build failed" >&2; git worktree remove --force "$WT" >/dev/null 2>&1; exit 2; }
  # The reference compiles this tree's corpus, so a test added by the change
  # under test is compared too (it is reported if the reference refuses it).
  rm -rf "$REFDIR"; mkdir -p "$REFDIR"
  # Only the programs: the reference keeps its own lib/, builtins and
  # package sources, which are part of what is being compared.
  cp -r test benchmark "$WT/" 2>/dev/null
  for d in packages/*/test; do mkdir -p "$WT/$d" && cp -r "$d/." "$WT/$d/"; done
  [ -f "$OC" ] && mkdir -p "$WT/build" && cp "$OC" "$WT/$OC"
  emit "$WT/bin/spinel" "$WT" "$REFDIR"
  for c in "$REFDIR"/*.c; do [ -f "$c" ] && LC_ALL=C sed -i "s|$WT|$ROOT|g" "$c"; done
  git worktree remove --force "$WT" >/dev/null 2>&1
  : > "$REFDIR/.done"
fi

NEWDIR=$(mktemp -d "${TMPDIR:-/tmp}/spinel-cident-new.XXXXXX")
emit "$NEW" "$ROOT" "$NEWDIR"

NORM='s/[0-9]{4}\.[0-9]{2}\.[0-9]{2}\+[0-9]+ revision [0-9a-f]+/REV/g'
same=0; diffn=0; refch=0; refused=0; fresh=0
for f in $(list); do
  key=$(printf "%s" "$f" | tr "/" "_")
  a=$REFDIR/$key; b=$NEWDIR/$key
  # a program added after the reference was cached has nothing to compare
  if [ ! -f "$a.c" ] && [ ! -f "$a.refused" ]; then fresh=$((fresh+1)); continue; fi
  if [ -f "$a.refused" ] && [ -f "$b.refused" ]; then refused=$((refused+1)); continue; fi
  if [ -f "$a.refused" ] || [ -f "$b.refused" ]; then
    refch=$((refch+1))
    if [ -f "$b.refused" ]; then echo "NOW REFUSED: $f"; else echo "NO LONGER REFUSED: $f"; fi
    continue
  fi
  # RUBY_DESCRIPTION names the compiler's own commit; a commit made after
  # the reference was cached changes it and nothing else
  if cmp -s "$a.c" "$b.c" ||
     cmp -s <(sed -E "$NORM" "$a.c") <(sed -E "$NORM" "$b.c"); then same=$((same+1))
  else
    diffn=$((diffn+1))
    echo "DIFFERS: $f"
    diff -u "$a.c" "$b.c" | head -20
  fi
done
rm -rf "$NEWDIR"
echo "cident: $same identical, $diffn differ, $refch refusal changes, $refused refused by both, $fresh not in the reference (against ${SHA:0:9})"
[ "$diffn" -eq 0 ] && [ "$refch" -eq 0 ] || exit 1
