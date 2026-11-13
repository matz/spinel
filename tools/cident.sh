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
# CIDENT_FLAGS adds compiler flags to both sides (e.g.
# CIDENT_FLAGS=--int-overflow=promote); the reference cache is kept per
# flag set.
#
# CIDENT_NAMES=1 also shows a change that only renames what the compiler
# invents (#9898 named appended nodes by origin): a program that differs is
# compared again with every identifier that holds a digit and that no
# runtime header declares numbered in the order it first appears, in code
# and in the symbol table's names (they spell local names), and with the
# symbol table's hash slots, which hash those names, left out once they are
# checked against the names. It counts as identical when only such names
# differ; a renamed runtime call, a number, a wrong hash slot, any other
# string literal or any other byte still differs.
#
# A reference of the same revision and flags built for another corpus is
# reused: only the programs the change of corpus can reach are compiled
# again. CIDENT_REF_ONLY=1 builds the reference and stops; CIDENT_KEEP (20)
# is how many references are kept.
#
# Exit status: 0 identical, 1 some file differs, 2 infrastructure error.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
REV=${1-}
[ -n "$REV" ] || { echo "usage: $0 <ref-rev>" >&2; exit 2; }
SHA=$(git rev-parse --verify -q "$REV^{commit}") || { echo "cident: unknown revision $REV" >&2; exit 2; }
JOBS=${CIDENT_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}
NEW=$ROOT/bin/spinel
[ -x "$NEW" ] || { echo "cident: build bin/spinel first" >&2; exit 2; }

# The corpus, as paths relative to the tree root.
OC=build/optcarrot-single.rb
list() {
  ls test/*.rb benchmark/*.rb packages/*/test/*.rb 2>/dev/null
  # the String-sharing programs share-strings-test compiles, which exercise
  # what the corpus above reaches least (stage 2 of the one representation
  # takes the share analysis apart under them)
  find test/share -name '*.rb' 2>/dev/null | LC_ALL=C sort
  [ -f "$OC" ] && echo "$OC"
}

# emit <spinel> <tree-root> <outdir>: C for every corpus file, or a .refused
# marker where the compiler refuses it. Paths stay relative so that the
# compiler's own tree resolves lib/ and packages/.
emit() {
  local sp=$1 tree=$2 out=$3 only=${4-}
  mkdir -p "$out"
  { if [ -n "$only" ]; then cat "$only"; else list; fi; } | xargs -P "$JOBS" -I{} sh -c '
    f="$1"; sp="$2"; tree="$3"; out="$4"
    key=$(printf "%s" "$f" | tr "/" "_")
    if (cd "$tree" && ulimit -v 16000000 2>/dev/null; $CIDENT_TO "$sp" -c --no-line-map $CIDENT_FLAGS "$f" -o "$out/$key.c" >/dev/null 2>&1); then :
    else rm -f "$out/$key.c"; : > "$out/$key.refused"; fi
  ' _ {} "$sp" "$tree" "$out"
}

CIDENT_FLAGS=${CIDENT_FLAGS-}
export CIDENT_FLAGS
# A compiler that never finishes a program (the reference compiler on a test
# written for a fix it lacks: #7882) held a lane for an hour and 20 GB. Each
# compile gets a time and a memory limit; one that hits it counts as refused.
CIDENT_TO=
for t in timeout gtimeout; do command -v $t >/dev/null 2>&1 && { CIDENT_TO="$t ${CIDENT_TIMEOUT:-300}"; break; }; done
export CIDENT_TO
FLAGKEY=$(printf "%s" "$CIDENT_FLAGS" | tr -c 'A-Za-z0-9=' '_')
# Include supporting files as well as entry points: require_relative and
# compile-time reads can change a program without changing its own bytes.
CORPUS=$(find test benchmark packages/*/test -type f 2>/dev/null; [ -f "$OC" ] && echo "$OC")
CORPUS=$(printf '%s\n' "$CORPUS" | LC_ALL=C sort)
CORPUSKEY=$({
  printf '%s\n' "$CORPUS"
  printf '%s\n' "$CORPUS" | git hash-object --stdin-paths
} | git hash-object --stdin)
# r2: the reference C as this cident leaves it, where a literal holding the
# tree's path carries the hash of its renamed bytes. A reference an older
# cident cached has another name, so it is neither used nor reused.
REFBASE=$ROOT/build/cident/$SHA-r2${FLAGKEY:+-$FLAGKEY}
REFDIR=$REFBASE-$CORPUSKEY
# A cold cache has one builder. Hold the lock through comparison so no
# other run can remove or read its outputs while they are being written.
# The lock is flock(2) on fd 9, which the kernel drops once no process
# holds that descriptor any more, however the holder ended: a run killed
# with SIGKILL leaves no stale lock, and compiles it orphaned keep the
# cache locked only until they exit. macOS has no flock(1); perl has flock.
LOCK=$REFDIR.lock
mkdir -p "$(dirname "$REFDIR")"
exec 9>"$LOCK"
cident_lock() {
  perl -MFcntl=:flock -e 'open(my $fh, ">&=", 9) or die "cident: cannot lock: $!\n";
    flock($fh, LOCK_EX) or die "cident: cannot lock: $!\n"'
}
cident_lock || exit 2
trap 'exit 2' HUP INT TERM
if [ ! -f "$REFDIR/.done" ]; then
  # The C embeds the compiler's own tree path in a few string literals,
  # together with their lengths. A reference tree at a path of the same
  # length as this one lets a plain rename make those bytes equal.
  L=${#ROOT}
  # the process id keeps two runs against the same revision (from two
  # checkouts whose paths have the same length) out of each other's tree.
  # The compiler embeds the resolved path, so /tmp is resolved too (it is
  # /private/tmp on macOS).
  TMPD=$(cd /tmp && pwd -P)
  WT=$(printf "%s/cident-%s-%s%0100d" "$TMPD" "$$" "$SHA" 0 | cut -c1-"$L")
  [ "$L" -ge 20 ] && [ ${#WT} -eq "$L" ] || { echo "cident: cannot place a reference tree beside $ROOT" >&2; exit 2; }
  git worktree remove --force "$WT" >/dev/null 2>&1; rm -rf "$WT"
  git worktree add --detach "$WT" "$SHA" >/dev/null 2>&1 || { echo "cident: cannot check out $REV" >&2; exit 2; }
  # -L: a vendor that is a symlink into another checkout is copied as its
  # files; copied as the link, it dangled from /tmp and the reference built
  # without the RBS extractor, so every inline-RBS program differed
  [ -d "$ROOT/vendor" ] && [ ! -d "$WT/vendor" ] && cp -rL "$ROOT/vendor" "$WT/vendor"
  # bundled-cext: the default target leaves the bundled C extensions
  # (packages/sqlite3, packages/racc) unbuilt, and a reference without them
  # refuses every program that requires one
  ( cd "$WT" && make -j"$JOBS" -s >/dev/null 2>&1 &&
    { ! grep -q '^bundled-cext:' Makefile || make -j"$JOBS" -s bundled-cext >/dev/null 2>&1; } ) || {
    echo "cident: reference build failed" >&2; git worktree remove --force "$WT" >/dev/null 2>&1; exit 2; }
  # The reference compiles this tree's corpus, so a test added by the change
  # under test is compared too (it is reported if the reference refuses it).
  rm -rf "$REFDIR"; mkdir -p "$REFDIR"
  # The corpus as the reference saw it: each file's blob and path
  MANIFEST=$(mktemp "${TMPDIR:-/tmp}/spinel-cident-man.XXXXXX")
  paste -d' ' <(printf '%s\n' "$CORPUS" | git hash-object --stdin-paths) <(printf '%s\n' "$CORPUS") > "$MANIFEST"
  # A reference of the same revision and flags for another corpus (each
  # cycle's tests change it) keeps the C of every program the change of
  # corpus cannot reach: only a program that is new or changed, or that names
  # a file that is new or changed (require_relative, a compile-time read), is
  # compiled again. Its .expected files are not read by the compiler.
  # (only this flag set's: the corpus key is hex, and another set's name
  # goes on with its flags, `<sha>-__int_overflow=promote-<key>`)
  # The pattern opens with its own parenthesis: bash 3.2, macOS's /bin/bash,
  # otherwise ends the command substitution at the pattern's closing one.
  PREV=$(ls -dt "$REFBASE"-*/ 2>/dev/null | while read -r d; do
           d=${d%/}; case ${d#"$REFBASE"-} in (*[!0-9a-f]*) continue;; esac
           [ "$d" != "$REFDIR" ] && [ -f "$d/.done" ] && [ -f "$d/.manifest" ] && { echo "$d"; break; }; done)
  ONLY=
  if [ -n "$PREV" ]; then
    ONLY=$(mktemp "${TMPDIR:-/tmp}/spinel-cident-only.XXXXXX")
    # added, changed or removed, either side of the two manifests
    CHANGED=$(LC_ALL=C comm -3 <(LC_ALL=C sort "$PREV/.manifest") <(LC_ALL=C sort "$MANIFEST") |
              sed 's/^\t//' | cut -d' ' -f2- | grep -v '\.expected$' | LC_ALL=C sort -u)
    {
      list | LC_ALL=C sort > "$ONLY.all"
      printf '%s\n' "$CHANGED" | LC_ALL=C comm -12 - "$ONLY.all"
      # a program that names the file, or a directory it is in (a
      # require_relative, a literal read, an autoload directory)
      printf '%s\n' "$CHANGED" | while read -r ch; do
        [ -n "$ch" ] || continue
        b=$(basename "$ch"); set -- "${b%.*}"
        for part in $(dirname "$ch" | tr '/' ' '); do
          case $part in test|benchmark|packages|.) ;; *) set -- "$@" "$part";; esac
        done
        for w in "$@"; do grep -lF -- "$w" $(cat "$ONLY.all") 2>/dev/null; done
      done
      rm -f "$ONLY.all"
    } | LC_ALL=C sort -u > "$ONLY"
    # the rest from the earlier reference, which keeps their C as it was
    list | LC_ALL=C sort | LC_ALL=C comm -23 - "$ONLY" | while read -r f; do
      key=$(printf "%s" "$f" | tr "/" "_")
      [ -f "$PREV/$key.c" ] && cp "$PREV/$key.c" "$REFDIR/"
      [ -f "$PREV/$key.refused" ] && cp "$PREV/$key.refused" "$REFDIR/"
    done
    echo "cident: reference C of $(git rev-parse --short "$SHA") reused from an earlier corpus, $(grep -c . "$ONLY") program(s) compiled again" >&2
  fi
  # Only the programs: the reference keeps its own lib/, builtins and
  # package sources, which are part of what is being compared.
  cp -r test benchmark "$WT/" 2>/dev/null
  for d in packages/*/test; do mkdir -p "$WT/$d" && cp -r "$d/." "$WT/$d/"; done
  [ -f "$OC" ] && mkdir -p "$WT/build" && cp "$OC" "$WT/$OC"
  emit "$WT/bin/spinel" "$WT" "$REFDIR" "$ONLY"
  # The reference tree's path has this tree's length, so renaming it makes
  # the bytes equal. A frozen literal's header also holds the FNV-1a hash of
  # its bytes (fzl_str_hash, #11516). Where that hash is the one of the bytes
  # as written, the literal gets the hash of its renamed bytes; any other
  # hash is left as written. Only here are both byte strings at hand: once
  # renamed, the reference no longer holds the bytes its hash was taken of.
  # A file copied from a reused reference names $ROOT already and is skipped.
  # The rename writes a .tmp file, so one that fails leaves the file whole
  # and stops cident.
  for c in "$REFDIR"/*.c; do
    [ -f "$c" ] && grep -qF "$WT" "$c" || continue
    CIDENT_WT=$WT CIDENT_ROOT=$ROOT LC_ALL=C perl -pe '
      BEGIN { ($wt, $rt) = @ENV{qw(CIDENT_WT CIDENT_ROOT)}; %ce = (n => "\n", t => "\t", r => "\r", "\\" => "\\", "\"" => "\"") }
      sub dec { my ($e, $o) = (shift, "");
        while ($e =~ /\G(?:\\([0-3][0-7]{2})|\\(.)|([^\\]+))/gs) {
          if (defined $1) { $o .= chr oct $1 } elsif (defined $2) { return undef unless exists $ce{$2}; $o .= $ce{$2} } else { $o .= $3 } }
        $o }
      sub fnv { my ($hi, $lo) = (0xcbf29ce4, 0x84222325);
        for my $b (unpack "C*", shift) { $lo ^= $b; my $l = $lo * 0x1b3;
          ($hi, $lo) = (($hi * 0x1b3 + ($l >> 32) + ($lo << 8)) & 0xffffffff, $l & 0xffffffff) }
        my $v = ($hi << 32) | $lo; $v ? $v : 1 }
      if (index($_, $wt) >= 0) {
        s#, (\d+), 0x([0-9a-f]+)ULL \}, 0xf1, "((?:[^"\\]|\\.)*)" \};# my ($n, $h, $e) = ($1, $2, $3); my $d = dec($e);
          if (defined $d && sprintf("%x", fnv($d)) eq $h) { (my $r = $e) =~ s/\Q$wt\E/$rt/g; $h = sprintf("%x", fnv(dec($r))) }
          ", $n, 0x${h}ULL }, 0xf1, \"$e\" };" #ge;
        s/\Q$wt\E/$rt/g }' < "$c" > "$c.tmp" && mv "$c.tmp" "$c" || {
      echo "cident: cannot rename the reference tree in $c" >&2
      rm -f "$MANIFEST" ${ONLY:+"$ONLY"}; git worktree remove --force "$WT" >/dev/null 2>&1; exit 2; }
  done
  git worktree remove --force "$WT" >/dev/null 2>&1
  mv "$MANIFEST" "$REFDIR/.manifest"; [ -n "$ONLY" ] && rm -f "$ONLY"
  : > "$REFDIR/.done"
  # Keep the newest references (CIDENT_KEEP, 20): each holds the C of the
  # whole corpus, and a lane machine filled its disk with them. One another
  # run holds the lock of is left alone.
  ls -dt "$ROOT"/build/cident/*/ 2>/dev/null | tail -n +"$(( ${CIDENT_KEEP:-20} + 1 ))" | while read -r d; do
    d=${d%/}
    [ "$d" = "$REFDIR" ] && continue
    perl -MFcntl=:flock -e 'open(my $fh, ">>", $ARGV[0]) or exit 1; flock($fh, LOCK_EX|LOCK_NB) or exit 1;
      system("rm", "-rf", $ARGV[1]); unlink $ARGV[0]' "$d.lock" "$d"
  done
fi

# CIDENT_REF_ONLY=1: only the reference, cached for the runs to come (built
# right after a push, for the master the next change is compared with)
[ -n "${CIDENT_REF_ONLY-}" ] && { echo "cident: reference of $(git rev-parse --short "$SHA") ready"; exit 0; }

NEWDIR=$(mktemp -d "${TMPDIR:-/tmp}/spinel-cident-new.XXXXXX")
emit "$NEW" "$ROOT" "$NEWDIR"

fzl_seq() { perl -pe 'BEGIN { %n = (); $k = 0 } s/\b_fzl_([0-9a-f]{8}(?:_[0-9a-f]+)?)(?![0-9a-zA-Z])/"_fzl_q" . ($n{$1} \/\/= $k++)/ge'; }
NORM='s/[0-9]{4}\.[0-9]{2}\.[0-9]{2}\+[0-9]+ revision [0-9a-f]+/REV/g'
RTIDS=
if [ -n "${CIDENT_NAMES-}" ]; then
  RTIDS=$(mktemp "${TMPDIR:-/tmp}/spinel-cident-rt.XXXXXX")
  find lib -name '*.h' -exec cat {} + | perl -ne 'print "$_\n" for /\b[A-Za-z_]\w*/g' | LC_ALL=C sort -u > "$RTIDS"
fi
export RTIDS
# names_seq: the comparison CIDENT_NAMES=1 makes. The symbol table's hash
# slots are rebuilt from the file's own sp_sym_names with codegen's layout
# (sym_hash_n; the smallest power of two from 8 up that is at least twice the
# names; linear probing; hash << 32 | id + 1) and left out only when they
# match (a name holding a NUL is read from its _sym_N object); a table that
# does not match, or whose names cannot all be read, gets a mark no other
# file has, so the file differs. Identifiers are then renamed in code and
# comments, and inside string literals only in the sp_sym_names initializer,
# which spells local names; every other literal is compared byte for byte.
names_seq() { perl -0777 -pe '
  BEGIN { open my $f, "<", $ENV{RTIDS} or die; chomp(my @w = <$f>); %rt = map { $_ => 1 } @w; %n = (); $k = 0;
    %ce = (n => "\n", t => "\t", r => "\r", a => "\a", b => "\b", f => "\f", v => "\x0b") }
  sub cesc { my $e = shift; $e =~ /^[0-7]/ ? chr(oct $e) : $e =~ /^x/ ? chr(hex substr($e, 1)) : $ce{$e} // $e }
  sub cstr { my ($s, $o) = (shift, ""); while ($s =~ /"((?:[^"\\]|\\.)*)"/g) { my $t = $1;
      $t =~ s/\\([0-7]{1,3}|x[0-9a-fA-F]+|.)/cesc($1)/ge;
      $o .= $t } $o }
  sub symh { my $h = 2166136261; $h = (($h ^ $_) * 16777619) & 0xffffffff for unpack "C*", shift;
    $h ^= $h >> 16; $h = ($h * 0x7feb352d) & 0xffffffff; $h ^ ($h >> 15) }
  sub ren { my $w = shift; $w =~ /\d/ && !$rt{$w} ? "N" . ($n{$w} //= $k++) : $w }
  sub ren_all { my $t = shift; $t =~ s/(?<![\\\w])([A-Za-z_]\w*)/ren($1)/ge; $t }
  my $ok = 0;
  if (/sp_sym_names\[(\d+)\] = \{((?:"(?:[^"\\]|\\.)*"|[^"])*?)\};/) {
    my ($ns, $init, @nm) = ($1, $2);
    my %bin; $bin{$1} = substr(cstr($3), 0, $2) while /_sym_(\d+) = \{ \{ NULL, \d+, (\d+), 0 \}, 0xf1, ((?:"(?:[^"\\]|\\.)*")+) \};/g;
    while ($init =~ /\(&\(((?:\s*"(?:[^"\\]|\\.)*")+)\)\[1\]\)|_sym_(\d+)\.d/g) { push @nm, defined $2 ? $bin{$2} : substr(cstr($1), 1) }
    if (@nm == $ns && !grep { !defined } @nm) {
      my $hsz = 8; $hsz <<= 1 while $hsz < 2 * $ns;
      my @ht = (0) x $hsz;
      for my $i (0 .. $#nm) { my $h = symh($nm[$i]); my $j = $h & ($hsz - 1);
        $j = ($j + 1) & ($hsz - 1) while $ht[$j]; $ht[$j] = ($h << 32) | ($i + 1) }
      my @want = map { $_ ? sprintf("0x%016xull", $_) : "0" } @ht;
      $ok = /sp_sym_htab\[(\d+)\] = \{([^}]*)\}/ && $1 == $hsz && join(",", split /\s*,\s*/, $2) eq join(",", @want);
    }
  }
  if ($ok) { s/(sp_sym_htab\[\d+\] = \{)[^}]*\}/$1}/ }
  else { s/(sp_sym_htab\[\d+\] = \{)/$1 . "\/* not checked " . rand() . " *\/"/e }
  my ($s0, $e0) = /sp_sym_names\[\d+\] = \{(?:"(?:[^"\\]|\\.)*"|[^"])*?\};/ ? ($-[0], $+[0]) : (-1, -1);
  s{(/\*.*?\*/|//[^\n]*)|("(?:[^"\\\n]|\\.)*"|\x27(?:[^\x27\\\n]|\\.)*\x27)|(?<![\\\w])([A-Za-z_]\w*)}
   {defined $1 ? ren_all($1) : defined $2 ? ($-[0] >= $s0 && $-[0] < $e0 ? ren_all($2) : $2) : ren($3)}gse'; }
same=0; renamed=0; diffn=0; refch=0; refused=0; fresh=0
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
  # A frozen literal is named by a hash of its bytes (#10073); one that
  # holds a path (__FILE__, a file read in) is named after the tree it was
  # compiled in, which the reference's is not, though its bytes compare
  # equal once the path is rewritten above. The names are numbered in the
  # order they appear before the second comparison; the bytes still are
  # compared.
  if cmp -s "$a.c" "$b.c" ||
     cmp -s <(sed -E "$NORM" "$a.c" | fzl_seq) <(sed -E "$NORM" "$b.c" | fzl_seq); then same=$((same+1))
  elif [ -n "$RTIDS" ] &&
       cmp -s <(sed -E "$NORM" "$a.c" | fzl_seq | names_seq) <(sed -E "$NORM" "$b.c" | fzl_seq | names_seq); then
    same=$((same+1)); renamed=$((renamed+1))
    echo "RENAMED: $f"
  else
    diffn=$((diffn+1))
    echo "DIFFERS: $f"
    diff -u "$a.c" "$b.c" | head -20
  fi
done
rm -rf "$NEWDIR"
[ -n "$RTIDS" ] && rm -f "$RTIDS"
echo "cident: $same identical${RTIDS:+ ($renamed of them only renamed)}, $diffn differ, $refch refusal changes, $refused refused by both, $fresh not in the reference (against ${SHA:0:9})"
[ "$diffn" -eq 0 ] && [ "$refch" -eq 0 ] || exit 1
