#!/bin/sh
# incremental_test.sh -- an --incremental build is the binary a from-scratch
# build gives, and a local edit recompiles a bounded number of units.
#
#   tools/incremental_test.sh [-k K] [SPINEL]
#
# --incremental splits the generated C into units and takes the object of
# every unit whose text did not change from a cache (src/unit_cache.c,
# c_split_units in src/csplit.c). Two things can go wrong there without any
# build failing: a cached object that no longer matches what the compiler
# would make of the unit now, and a split so unstable that every edit misses
# every unit. So each check below compares the binary of a warm build with
# the binary of a build that compiled everything (SPINEL_NO_CACHE=1), byte
# for byte, and bounds the misses of the edits of tools/unit_stability.sh
# plus three that add a method, a literal and an instance variable. The
# rest pins the contract around it: the refused flags, the default build
# untouched, the shape of a unit directory, the planted missing declaration
# that must be a compile error and never an object, reproducible objects,
# a corpus smoke, two builds sharing an empty cache, the prune, the
# working directory in a -g build's key, a -g edit as local as a plain one
# (for an instance variable barrier and for a cell barrier), and a directory
# spinel did not write, or a file in a unit set it did not list, left alone.
#
# Every build runs with SPINEL_INCREMENTAL_STRICT=1, so a split or unit
# compile the fallback would hide fails here instead. The cache lives in
# the temp directory. Each check prints its numbers; a failing check is
# named again at the end.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
K=30
while [ $# -gt 0 ]; do
  case "$1" in
    -k) K=$2; shift 2 ;;
    *) break ;;
  esac
done
SP=${1:-$ROOT/bin/spinel}
case "$SP" in /*) ;; *) SP=$(pwd)/$SP ;; esac
[ -x "$SP" ] || { echo "incremental-test: build bin/spinel first" >&2; exit 2; }
T=$(mktemp -d "${TMPDIR:-/tmp}/spinel-incremental.XXXXXX") || exit 2
trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
# the compiler's own temp files too (the C, the work directories, the C a
# failed build leaves for inspection) go where the trap removes them
mkdir "$T/tmp" || exit 2
TMPDIR=$T/tmp
export TMPDIR
unset SPINEL_NO_CACHE SPINEL_KEEP_UNITS SPINEL_UNIT_DROP_DECL SPINEL_CACHE_DAYS SPINEL_INCREMENTAL 2>/dev/null
SPINEL_CACHE_DIR=$T/cache
SPINEL_INCREMENTAL_STRICT=1
export SPINEL_CACHE_DIR SPINEL_INCREMENTAL_STRICT
cd "$ROOT" || exit 2

fail=0
failed=""
# report CHECK OK DETAIL: one line per check, with the seconds since the
# previous one; the failures are listed again at the end
report() {
  if [ "$2" = 1 ]; then _v=ok; else _v=FAIL; fail=1; failed="$failed
  $1"; fi
  _now=$(date +%s)
  echo "incremental-test: $1: $3 $_v [$((_now - t_last))s]"
  t_last=$_now
}
t_last=$(date +%s)
# the fields of the last --cache-stats summary in a log
stats() {
  _line=$(grep '^spinel: cache: units=' "$1" 2>/dev/null | tail -1)
  units=$(field units); hit=$(field hit); miss=$(field miss); rtf=$(field rt)
  : "${units:=-1}" "${hit:=-1}" "${miss:=-1}" "${rtf:=-}"
}
field() { printf '%s\n' "$_line" | tr ' ' '\n' | sed -n "s/^$1=//p"; }
# every binary is linked as $T/o_NAME/prog and then moved to $T/NAME: the
# macOS linker signs a binary under its file name (and its LC_UUID follows),
# so two builds compared byte for byte must have been linked under one name
out() { mkdir -p "$T/o_$1"; echo "$T/o_$1/prog"; }
moved() { [ -f "$T/o_$1/prog" ] && mv "$T/o_$1/prog" "$T/$1"; return 0; }
# ib NAME SRC [FLAG...]: an --incremental --cache-stats build of SRC into
# $T/NAME, its log in $T/NAME.err, its exit status in rc, its stats parsed
ib() {
  _n=$1; _s=$2; shift 2
  "$SP" "$_s" --incremental --cache-stats "$@" -o "$(out "$_n")" > "$T/$_n.err" 2>&1
  rc=$?
  moved "$_n"
  stats "$T/$_n.err"
}
# ib_env VAR=VALUE... -- NAME SRC [FLAG...]: ib with those variables set for
# that one build (a VAR=VALUE prefix on a function call outlives the call in
# some shells, bash's sh among them)
ib_env() {
  ( while [ "$1" != -- ]; do export "$1"; shift; done; shift; ib "$@"; exit $rc )
  rc=$?
  while [ "$1" != -- ]; do shift; done
  stats "$T/$2.err"
}
# a build that dies says why in its last lines
tail_of() { [ "$rc" = 0 ] || tail -3 "$T/$1.err" | sed 's/^/    /'; }
same() { cmp -s "$1" "$2" && echo 1 || echo 0; }
if command -v timeout > /dev/null 2>&1; then run_bin() { timeout 60 "$@"; }; else run_bin() { "$@"; }; fi
if command -v sha256sum > /dev/null 2>&1; then sha() { sha256sum | cut -c1-64; }; else sha() { shasum -a 256 | cut -c1-64; }; fi
# touch_days FILE N: set FILE's mtime N days back (GNU and BSD date)
touch_days() {
  ts=$(date -d "$2 days ago" +%Y%m%d%H%M 2>/dev/null || date -v-"$2"d +%Y%m%d%H%M)
  touch -t "$ts" "$1"
}

# --- the scale program and its edits ---------------------------------------
sh test/scale/gen.sh "$K" > "$T/base.rb"
MID=$((K / 2))
edit() {  # edit NAME AWK-PROGRAM: the base with one change in class Model<MID>
  awk -v m="$MID" "\$0 == \"class Model\" m \" < Base\" {f=1} $2 {print}" "$T/base.rb" > "$T/$1.rb"
  cmp -s "$T/base.rb" "$T/$1.rb" && { echo "incremental-test: edit $1 did not apply (test/scale/gen.sh changed shape?)" >&2; exit 2; }
}
# the three of tools/unit_stability.sh
edit a 'f && /def label = /{sub(/-#\{@tags.size\}/, "+#{@tags.size}"); f=0}'
edit b 'f && /def weight = 2/{sub(/= 2/, "= 3"); f=0}'
edit c 'f && /def weight = 2/{sub(/= 2/, "= 2.5"); f=0}'
# a method appended to the class (and called once from the top level: a
# method nothing calls is not compiled, and the edit would never reach the
# C), a literal appended to a method body, an instance variable the class
# did not have (the program already sets @extra, so the new one is @extra2)
edit add-method 'f && /^end$/{print "  def extra = @id + 1"; f=0}'
printf 'p Model%d.new(0).extra\n' "$MID" >> "$T/add-method.rb"
edit add-literal 'f && /def label = /{sub(/"$/, "\" + \"!\""); f=0}'
edit add-ivar 'f && /@extra = nil/{print "    @extra2 = 0"; f=0}'
# a barrier more in a method, on its own line (under -g the markers keep
# every later unit's text as it was)
edit wb 'f && /^    @extra = v$/{sub(/@extra = v$/, "@extra = v; @label = \"m\""); f=0}'
# one more no build has stored, for the unwritable cache to refuse
edit d 'f && /def weight = 2/{sub(/= 2/, "= 4"); f=0}'
# every incremental build of the scale program reads the same path, so an
# edit differs from the base in its text and not in its file name
mkdir -p "$T/src"
cp "$T/base.rb" "$T/src/prog.rb"
P=$T/src/prog.rb

# a method the units call, for the header check and the planted gap: the
# scale program's describe, which the C names sp_Model0_describe in some
# sizes of the program and sp_Base_describe in others (K=30)
DESC=sp_Model0_describe
"$SP" "$T/base.rb" -c --no-line-map -o "$T/desc.c" > /dev/null 2>&1
grep -q "$DESC(" "$T/desc.c" || DESC=sp_Base_describe
rm -f "$T/desc.c"

# --- 1 refusals ------------------------------------------------------------
nref=0; bad=""
for fl in -S --print-build --emit-rbs --emit-types --emit-symbol-map "--ext-init x"; do
  # shellcheck disable=SC2086
  "$SP" --incremental $fl "$P" -o "$T/ref" > "$T/ref.err" 2>&1
  r=$?
  if [ $r = 1 ] && grep -q 'cannot be combined' "$T/ref.err"; then nref=$((nref + 1)); else bad="$bad [$fl rc=$r]"; fi
done
for fl in --unit-bytes=1M --cache-stats; do
  "$SP" $fl "$P" -o "$T/ref" > "$T/ref.err" 2>&1
  r=$?
  if [ $r = 1 ] && grep -q 'needs --incremental' "$T/ref.err"; then nref=$((nref + 1)); else bad="$bad [$fl alone rc=$r]"; fi
done
[ $nref = 8 ] && o=1 || o=0
report "1 refusals" $o "$nref of 8 refused$bad"

# --- 2 the default output is untouched -------------------------------------
"$SP" "$P" -c --no-line-map -o "$T/plain.c" > "$T/plain.c.err" 2>&1
r1=$?
SPINEL_INCREMENTAL=1 "$SP" "$P" -c --no-line-map -o "$T/plain2.c" > "$T/plain2.c.err" 2>&1
r2=$?
np=$(grep -c '^#pragma spinel' "$T/plain.c")
o=0; [ $r1 = 0 ] && [ $r2 = 0 ] && [ "$np" = 0 ] && cmp -s "$T/plain.c" "$T/plain2.c" && o=1
report "2 default C" $o "$np pragma lines, identical with SPINEL_INCREMENTAL=1 set: $(same "$T/plain.c" "$T/plain2.c")"

# --- 3 cold build, output --------------------------------------------------
# the plain build at -O0 is only the reference for the output; it runs
# beside the incremental one, whose compiles leave cores idle
{ "$SP" -O0 "$P" -o "$T/plain" > "$T/plain.err" 2>&1 && run_bin "$T/plain" > "$T/plain.out" 2>&1; } &
pplain=$!
ib b0 "$P"
wait $pplain
run_bin "$T/b0" > "$T/b0.out" 2>&1
o=0; [ $rc = 0 ] && [ "$miss" = "$units" ] && [ "$hit" = 0 ] && [ "$units" -gt 0 ] && cmp -s "$T/b0.out" "$T/plain.out" && o=1
report "3 cold build" $o "rc=$rc units=$units hit=$hit miss=$miss output=plain:$(same "$T/b0.out" "$T/plain.out")"
tail_of b0
UNITS0=$units

# --- 4 two cold builds are identical ---------------------------------------
ib_env SPINEL_CACHE_DIR="$T/cache4" -- b0x "$P"
o=0; [ $rc = 0 ] && [ "$miss" = "$units" ] && cmp -s "$T/b0" "$T/b0x" && o=1
report "4 second cold build" $o "rc=$rc miss=$miss/$units cmp=$(same "$T/b0" "$T/b0x")"
rm -rf "$T/cache4"

# --- 5 no-op rebuild -------------------------------------------------------
ib b1 "$P"
o=0; [ $rc = 0 ] && [ "$miss" = 0 ] && [ "$hit" = "$UNITS0" ] && cmp -s "$T/b0" "$T/b1" && o=1
report "5 no-op rebuild" $o "rc=$rc units=$units hit=$hit miss=$miss cmp=$(same "$T/b0" "$T/b1")"

# --- 8 (started here) --jobs=1 on an emptied cache ---------------------------
# one core for its whole length, so it runs beside the edits below, on a
# copy of the base in another directory and in a cache of its own (the
# directory a program is in reaches neither the units nor the binary,
# so it is still the binary b0 is)
mkdir -p "$T/s8"
cp "$T/base.rb" "$T/s8/prog.rb"
( SPINEL_CACHE_DIR=$T/cache8; export SPINEL_CACHE_DIR; ib b8 "$T/s8/prog.rb" --jobs=1 --timing; exit $rc ) &
p8=$!

# --- 6 edits: a warm build against a from-scratch one ----------------------
# Each edit is built warm from $P, then from scratch (SPINEL_NO_CACHE=1) from
# a copy in a directory of its own; the scratch build runs beside the next
# edit's warm build, whose frontend leaves the cores idle, and the edit is
# reported once both are done.
edit_warm() {  # edit_warm EDIT: the warm build, its counts kept for the report
  cp "$T/$1.rb" "$P"
  ib "w_$1" "$P"
  wrc=$rc; wmiss=$miss; wunits=$units; wrt=$rtf
}
edit_scratch() {  # edit_scratch EDIT: the from-scratch build, in the background
  mkdir -p "$T/s6/$1"
  cp "$T/$1.rb" "$T/s6/$1/prog.rb"
  ( SPINEL_NO_CACHE=1; export SPINEL_NO_CACHE; ib "s_$1" "$T/s6/$1/prog.rb"; exit $rc ) &
  ps6=$!
  ewrc=$wrc; ewmiss=$wmiss; ewunits=$wunits; ewrt=$wrt
}
# edit_report EDIT BOUND WHAT: bound is the most units the edit may miss
# (-1: no bound, but the runtime units must all hit); every edit must miss
# at least one, or it never reached the C
edit_report() {
  v=$1; bound=$2; what=$3
  wait $ps6; src=$?
  o=0
  if [ $ewrc = 0 ] && [ $src = 0 ] && cmp -s "$T/w_$v" "$T/s_$v" && [ "$ewmiss" -ge 1 ]; then
    if [ "$bound" -ge 0 ]; then [ "$ewmiss" -le "$bound" ] && o=1
    else r1=${ewrt%/*}; r2=${ewrt#*/}; [ "$r1" = "$r2" ] && [ "$r2" -gt 0 ] && o=1; fi
  fi
  lim="limit $bound"; [ "$bound" -ge 0 ] || lim="rt=$ewrt must be all hits"
  report "6 edit $v ($what)" $o "rc=$ewrc/$src miss=$ewmiss of $ewunits ($lim) cmp-scratch=$(same "$T/w_$v" "$T/s_$v")"
  [ $ewrc = 0 ] || { rc=$ewrc; tail_of "w_$v"; }
  [ $src = 0 ] || { rc=$src; tail_of "s_$v"; }
  grep '^spinel: cache: miss ' "$T/w_$v.err" | sed 's/^spinel: cache: /    /'
  rm -f "$T/s_$v"
}
edit_warm a; edit_scratch a
edit_warm b; edit_report a 2 "a literal in a body"; edit_scratch b
edit_warm c; edit_report b 1 "a constant a method returns"; edit_scratch c
edit_warm add-method; edit_report c 6 "a return type Integer -> Float"; edit_scratch add-method
edit_warm add-literal; edit_report add-method 3 "a method added"; edit_scratch add-literal
edit_warm add-ivar; edit_report add-literal 2 "a literal added"; edit_scratch add-ivar
edit_report add-ivar -1 "an instance variable added"
cp "$T/base.rb" "$P"

# --- 7 SPINEL_NO_CACHE ------------------------------------------------------
# through a C compiler that writes each command line down, and with the
# units kept, for case 13: the wrapper changes nothing the compiler sees,
# so the binary is still b0. Each compile writes a file of its own: the
# units compile in parallel, and their arguments appended to one file
# interleaved, so case 13 replayed a line made of two.
cat > "$T/cc-log" << EOF
#!/bin/sh
for a in "\$@"; do printf "'%s' " "\$a"; done > "$T/cc.log.\$\$"; echo >> "$T/cc.log.\$\$"
exec cc "\$@"
EOF
chmod +x "$T/cc-log"
ib_env SPINEL_NO_CACHE=1 SPINEL_KEEP_UNITS=1 -- b7 "$P" --cc="$T/cc-log"
w7=$(sed -n 's/^spinel: units kept at //p' "$T/b7.err")
o=0; [ $rc = 0 ] && [ "$miss" = "$UNITS0" ] && cmp -s "$T/b0" "$T/b7" && o=1
report "7 SPINEL_NO_CACHE=1" $o "rc=$rc miss=$miss of $units cmp=$(same "$T/b0" "$T/b7")"
tail_of b7

# --- 8 (reported) --------------------------------------------------------------
wait $p8; rc=$?
stats "$T/b8.err"
j=$(sed -n 's/^spinel-timing: phase=cc_compile .* jobs=\([0-9]*\)$/\1/p' "$T/b8.err")
o=0; [ $rc = 0 ] && [ "$miss" = "$UNITS0" ] && [ "$j" = 1 ] && cmp -s "$T/b0" "$T/b8" && o=1
report "8 --jobs=1 on an empty cache" $o "rc=$rc miss=$miss of $units jobs=$j cmp=$(same "$T/b0" "$T/b8")"
tail_of b8
rm -rf "$T/cache8"

# --- 9 an unwritable cache --------------------------------------------------
# root writes through any mode bits, so there the objects directory is
# replaced by a file instead, which no store can make a directory under
cp "$T/d.rb" "$P"
if [ "$(id -u)" = 0 ]; then
  mkdir -p "$T/rocache"; : > "$T/rocache/objects"
  ib_env SPINEL_CACHE_DIR="$T/rocache" -- b9 "$P"; how="objects is a file (root)"
else
  chmod -R a-w "$SPINEL_CACHE_DIR/objects"
  ib b9 "$P"; how="chmod a-w"
  chmod -R u+w "$SPINEL_CACHE_DIR/objects"
fi
sf=$(grep -c '^spinel: cache: store failed' "$T/b9.err")
o=0; [ $rc = 0 ] && [ "$sf" -ge 1 ] && [ -x "$T/b9" ] && o=1
report "9 unwritable cache" $o "rc=$rc ($how) store-failed lines=$sf"
tail_of b9
cp "$T/base.rb" "$P"

# --- 10 the shape of a unit directory ---------------------------------------
"$SP" "$P" -c --incremental -o "$T/u" > "$T/u.err" 2>&1; r1=$?
"$SP" "$P" -c --incremental -o "$T/u2" > "$T/u2.err" 2>&1; r2=$?
U=$T/u
nu=$(ls "$U"/unit_*.c 2>/dev/null | wc -l | tr -d ' ')
npr=$(cat "$U"/unit_*.c | grep -c '^#pragma')
# a method is declared by the units that call it, never by the header
nproto=$(grep -c "$DESC(" "$U/sp_program.h")
nuse=$(cat "$U"/unit_*.c | grep -c "$DESC(")
# a function body in the header is an inline one (a line-0 head with a
# parameter list and a brace; the attribute line of a cold helper counts)
nbody=$(awk '(/^[A-Za-z_][^=;]*\)[ \t]*\{/ || /^ __attribute__[^=;]*\)[ \t]*\{/) && !/^(typedef|struct|union|enum|extern|if|for|while|switch|do|else|return)[ \t(]/ {
    if ($0 !~ /inline/ && prev !~ /inline/) { bad++; print FILENAME ":" NR ": " substr($0, 1, 100) > "/dev/stderr" } }
  { if ($0 !~ /^#/ && $0 !~ /^[ \t]*$/) prev = $0 } END { print bad + 0 }' "$U/sp_program.h" 2>"$T/u.bodies")
# _sp_gcr_ numbers restart in every function (the awk of the step 3 check)
gaps=$(for u in "$U"/unit_*.c "$U/sp_program.h" "$U/sp_rt.h"; do
  awk '/^[^# ].*\) *\{$/{n=0} /_sp_gcr_[0-9]+/{for(i=1;i<=NF;i++) if ($i ~ /_sp_gcr_[0-9]+/) {match($i,/_sp_gcr_[0-9]+/); v=substr($i,RSTART+8,RLENGTH-8)+0; if (v>n+1) print FILENAME": gap at "v; if (v>n) n=v}}' "$u"
done | wc -l | tr -d ' ')
ngcr=$(cat "$U"/unit_*.c | grep -c '_sp_gcr_')
lines=$(awk 'NR>2' "$U/units.txt" | grep -c '')
dupl=$(awk 'NR>2{print $1}' "$U/units.txt" | sort | uniq -d | wc -l | tr -d ' ')
dupf=$(awk 'NR>2{print $2}' "$U/units.txt" | sort | uniq -d | wc -l | tr -d ' ')
hmis=0
for u in "$U"/unit_*.c; do
  h=$(grep -v '^# [0-9]' "$u" | sha)
  grep -q " $h " "$U/units.txt" || hmis=$((hmis + 1))
done
diff -r "$U" "$T/u2" > /dev/null 2>&1 && det=1 || det=0
# __COUNTER__ as a macro argument, so a comment that names it does not count
nctr=$(grep -c '[(,][ 	]*__COUNTER__[ 	]*)' lib/sp_gc.h)
other=$(grep -rl --include='*.h' '[(,][ 	]*__COUNTER__[ 	]*)' lib | grep -v '^lib/sp_gc.h$' | wc -l | tr -d ' ')
o=0
[ $r1 = 0 ] && [ $r2 = 0 ] && [ "$nu" -gt 0 ] && [ "$nu" = "$lines" ] && [ "$npr" = 0 ] && [ "$nproto" = 0 ] &&
  [ "$nuse" -gt 0 ] && [ "$nbody" = 0 ] && [ "$gaps" = 0 ] && [ "$dupl" = 0 ] && [ "$dupf" = 0 ] &&
  [ "$hmis" = 0 ] && [ $det = 1 ] && [ "$nctr" = 3 ] && [ "$other" = 0 ] && o=1
report "10 unit directory" $o "units=$nu manifest=$lines pragmas=$npr header-method-protos=$nproto (unit uses $nuse) header-non-inline-bodies=$nbody gcr-gaps=$gaps (of $ngcr) dup-labels=$dupl dup-files=$dupf hash-mismatches=$hmis deterministic=$det __COUNTER__=$nctr+$other"
[ "$nbody" = 0 ] || head -3 "$T/u.bodies" | sed 's/^/    /'

# --- 11 an over-cap group ----------------------------------------------------
# a class over a 32K unit, a Fiber and capturing blocks in main: every
# segment of a tag (the capture structs too) must stay with it
sh test/scale/gen.sh 60 > "$T/big.rb"
cat >> "$T/big.rb" << 'EOF'
base = acc % 7
fib = Fiber.new { |x| y = Fiber.yield(x + base); y * 2 }
p fib.resume(1), fib.resume(5)
add = ->(v) { v + base }
p [1, 2, 3].map { |v| v * base + add.call(v) }
EOF
{ "$SP" -O0 "$T/big.rb" -o "$T/big_plain" > "$T/big_plain.err" 2>&1 && run_bin "$T/big_plain" > "$T/big_plain.out" 2>&1; } &
pplain=$!
ib_env SPINEL_CACHE_DIR="$T/cache11" SPINEL_KEEP_UNITS=1 -- big "$T/big.rb" --unit-bytes=32K
wait $pplain
kept=$(sed -n 's/^spinel: units kept at //p' "$T/big.err")
nx=0; [ -n "$kept" ] && nx=$(awk 'NR>2 && $1 ~ /^x_/' "$kept/units.txt" | wc -l | tr -d ' ')
run_bin "$T/big" > "$T/big.out" 2>&1
o=0; [ $rc = 0 ] && [ "$nx" -ge 1 ] && cmp -s "$T/big.out" "$T/big_plain.out" && o=1
report "11 over-cap groups (--unit-bytes=32K)" $o "rc=$rc units=$units x_-units=$nx output=plain:$(same "$T/big.out" "$T/big_plain.out")"
tail_of big
[ -n "$kept" ] && rm -rf "$kept"
rm -rf "$T/cache11"

# --- 12 the planted gap -------------------------------------------------------
# one declaration left out of the table: implicit declarations are errors in
# a unit compile, so this is a failed unit, never a cached object
ib_env SPINEL_UNIT_DROP_DECL="$DESC" -- gap "$P"
# gcc says "implicit declaration of function", clang "call to undeclared
# function"; both name the warning they turned into an error
imp=$(grep -c 'implicit-function-declaration' "$T/gap.err")
stf=$(grep -c '^spinel: incremental: unit .* did not compile; STRICT, failing' "$T/gap.err")
o=0; [ $rc = 1 ] && [ "$imp" -ge 1 ] && [ "$stf" = 1 ] && o=1
report "12 planted gap, STRICT" $o "$DESC: rc=$rc implicit-declaration=$imp strict-notice=$stf"
# without STRICT the build falls back to the single unit, whose one long
# compile runs beside cases 13 and 14 and is reported after them
( unset SPINEL_INCREMENTAL_STRICT; ib_env SPINEL_UNIT_DROP_DECL="$DESC" -- gapfb "$P"; exit $rc ) &
p12=$!

# --- 13 reproducible objects ---------------------------------------------------
# a program unit's compile line, as case 7's build ran it, run again in two
# other directories: the same object as the build's
w13=$w7
pf=$(awk 'NR>2 && $4 == "program" {print $2; exit}' "$w13/units.txt" 2>/dev/null)
o=0; detail="no program unit kept"
if [ -n "$pf" ]; then
  cl=$(cat "$T"/cc.log.* | grep -F "'$w13/$pf'" | grep -- '-frandom-seed=' | head -1)
  mkdir -p "$T/r1" "$T/r2/deeper"
  cp -R "$w13" "$T/r1/w"; cp -R "$w13" "$T/r2/deeper/w"
  for d in "$T/r1/w" "$T/r2/deeper/w"; do
    rm -f "$d/${pf%.c}.o"
    l=$(printf '%s\n' "$cl" | sed "s|$w13|$d|g")
    eval "cc $l" > "$T/r.err" 2>&1
  done
  o1=$T/r1/w/${pf%.c}.o; o2=$T/r2/deeper/w/${pf%.c}.o
  [ -n "$cl" ] && [ -s "$o1" ] && cmp -s "$o1" "$o2" && cmp -s "$o1" "$w13/${pf%.c}.o" && o=1
  detail="unit=$pf logged=$([ -n "$cl" ] && echo 1 || echo 0) cmp-two-dirs=$(same "$o1" "$o2") cmp-build=$(same "$o1" "$w13/${pf%.c}.o")"
fi
report "13 reproducible unit object" $o "$detail"
[ -n "$w13" ] && rm -rf "$w13"

# --- 14 corpus smoke -----------------------------------------------------------
# each program cold, then warm: the output against .expected both times,
# no miss warm, the same binary; one shared cache (programs with the same
# runtime defines share the runtime units)
export SPINEL_CACHE_DIR=$T/cache14
smoke() {  # smoke NAME FILE EXPECTED [FLAG...]
  n=$1; f=$2; e=$3; shift 3
  args=""; [ -f "$f.args" ] && args=$(cat "$f.args")
  in=/dev/null; [ -f "$f.stdin" ] && in=$f.stdin
  r=""; ok14=1
  for k in cold warm; do
    ib "sm_$n.$k" "$f" "$@"
    # shellcheck disable=SC2086
    [ $rc = 0 ] && (run_bin "$T/sm_$n.$k" $args < "$in" > "$T/sm_$n.$k.out" 2>/dev/null)
    cmp -s "$T/sm_$n.$k.out" "$e" && r="$r $k=ok" || { r="$r $k=OUTPUT-DIFFERS"; ok14=0; }
    [ $rc = 0 ] || { r="$r(rc=$rc)"; ok14=0; tail_of "sm_$n.$k"; }
    [ $k = warm ] && { r="$r miss=$miss/$units"; [ "$miss" = 0 ] || ok14=0; }
  done
  if [ "${SMOKE_NO_CMP:-}" = 1 ]; then r="$r (-g: no identity claim)"
  elif cmp -s "$T/sm_$n.cold" "$T/sm_$n.warm"; then r="$r cmp=1"; else r="$r cmp=0"; ok14=0; fi
  report "14 smoke $n" $ok14 "${r# }"
  rm -f "$T/sm_$n.cold" "$T/sm_$n.warm"
}
for t in proc_return_scopes post_execution fiber_kill fiber_transfer_root poly_thread_join_timeout \
         raise_reraise_in_def regexp_gsub_zero_width struct_block_declarations marshal_unset_ivar \
         dispatch_override_param_list catch_throw_through_loop ffi_const; do
  smoke $t "test/$t.rb" "test/$t.rb.expected"
done
printf '18446744073709551623\n36893488147419103232\n' > "$T/promote.exp"
printf '7\n0\n' > "$T/wrap.exp"
smoke overflow-promote test/infer/split_build_overflow_mode.rb "$T/promote.exp" --int-overflow=promote
smoke overflow-wrap test/infer/split_build_overflow_mode.rb "$T/wrap.exp" --int-overflow=wrap
SMOKE_NO_CMP=1
smoke fiber_kill-debug test/fiber_kill.rb test/fiber_kill.rb.expected --debug
SMOKE_NO_CMP=
rm -rf "$T/cache14"

# --- 12 (reported) ---------------------------------------------------------------
wait $p12; rc=$?
fb=$(grep -c '^spinel: incremental: unit .* did not compile; compiling as one unit' "$T/gapfb.err")
run_bin "$T/gapfb" > "$T/gapfb.out" 2>&1
o=0; [ $rc = 0 ] && [ "$fb" = 1 ] && cmp -s "$T/gapfb.out" "$T/plain.out" && o=1
report "12 planted gap, fallback" $o "rc=$rc fallback-notice=$fb output=plain:$(same "$T/gapfb.out" "$T/plain.out")"
tail_of gapfb

# --- 15 two builds on one empty cache -----------------------------------------
export SPINEL_CACHE_DIR=$T/cache15
"$SP" "$P" --incremental --cache-stats -o "$(out c15a)" > "$T/c15a.err" 2>&1 &
pa=$!
"$SP" "$P" --incremental --cache-stats -o "$(out c15b)" > "$T/c15b.err" 2>&1 &
pb=$!
wait $pa; ra=$?
wait $pb; rb=$?
moved c15a; moved c15b
ib c15c "$P"
ntmp=$(find "$T/cache15" -name '*.tmp.*' | wc -l | tr -d ' ')
o=0; [ $ra = 0 ] && [ $rb = 0 ] && [ $rc = 0 ] && cmp -s "$T/c15a" "$T/c15b" && cmp -s "$T/c15a" "$T/b0" &&
  [ "$miss" = 0 ] && [ "$ntmp" = 0 ] && o=1
report "15 concurrent cold builds" $o "rc=$ra/$rb cmp=$(same "$T/c15a" "$T/c15b") cmp-b0=$(same "$T/c15a" "$T/b0") third-miss=$miss tmp-files=$ntmp"

# --- 16 prune -------------------------------------------------------------------
# an object no build used for 20 days and a store left behind two days ago
# go; the stamp says the walk ran; what this build hit stays
O=$T/cache15/objects
mkdir -p "$O/ab"
old=$O/ab/ab0000000000000000000000000000ff.o
echo x > "$old"; touch_days "$old" 20
stale=$O/ab/ab0000000000000000000000000000fe.o.tmp.1
echo x > "$stale"; touch_days "$stale" 2
touch_days "$O/.pruned" 2
nobj0=$(find "$O" -name '*.o' | wc -l | tr -d ' ')
ib c16 "$P"
nobj1=$(find "$O" -name '*.o' | wc -l | tr -d ' ')
fresh=0; [ -n "$(find "$O/.pruned" -mtime -1 2>/dev/null)" ] && fresh=1
o=0; [ $rc = 0 ] && [ ! -e "$old" ] && [ ! -e "$stale" ] && [ $fresh = 1 ] && [ "$miss" = 0 ] && [ "$nobj1" = $((nobj0 - 1)) ] && o=1
gone=0; [ -e "$old" ] || gone=1
report "16 prune" $o "rc=$rc old-object-removed=$gone stale-tmp-removed=$([ -e "$stale" ] && echo 0 || echo 1) stamp-rewritten=$fresh objects=$nobj0->$nobj1 miss=$miss"

# --- 17 -g in two directories ----------------------------------------------------
# under -g an object records the directory cc ran in, which the markers'
# relative paths resolve against: the same app.rb built from another
# checkout on one cache compiles its own objects, and hits them next time
export SPINEL_CACHE_DIR=$T/cache17
mkdir -p "$T/d17a" "$T/d17b"
cp test/raise_reraise_in_def.rb "$T/d17a/app.rb"; cp test/raise_reraise_in_def.rb "$T/d17b/app.rb"
cd "$T/d17a" && ib g17a app.rb -g; ra=$rc
cd "$T/d17b" && ib g17b app.rb -g; rb=$rc; mb=$miss; ub=$units
ib g17c app.rb -g
cd "$ROOT" || exit 2
o=0; [ $ra = 0 ] && [ $rb = 0 ] && [ $rc = 0 ] && [ "$ub" -gt 0 ] && [ "$mb" = "$ub" ] && [ "$miss" = 0 ] && o=1
report "17 -g in two directories" $o "rc=$ra/$rb/$rc other-dir-miss=$mb/$ub same-dir-miss=$miss"

# --- 18 -g barrier temps -----------------------------------------------------------
# under -g a method is not static, and the barrier temps of the methods
# after it ran on from its count: the edit wb changes no more units of a
# -g unit set than of a plain one
mkdir -p "$T/s18"
uset() {  # uset SRC NAME [FLAG...]: the sorted unit hashes of SRC's unit set
  _s=$1; _n=$2; shift 2
  cp "$T/$_s.rb" "$T/s18/prog.rb"
  "$SP" "$T/s18/prog.rb" -c --incremental "$@" -o "$T/$_n" > "$T/$_n.err" 2>&1 || echo "$_n failed" >&2
  awk 'NR>2{print $3}' "$T/$_n/units.txt" 2>/dev/null | sort > "$T/$_n.h"
}
uset base u18p; uset wb u18pw; uset base u18g -g; uset wb u18gw -g
cp_=$(comm -13 "$T/u18p.h" "$T/u18pw.h" | wc -l | tr -d ' ')
cg_=$(comm -13 "$T/u18g.h" "$T/u18gw.h" | wc -l | tr -d ' ')
o=0; [ "$cp_" -ge 1 ] && [ "$cg_" -ge 1 ] && [ "$cg_" -le "$cp_" ] && o=1
report "18 -g barrier temps" $o "changed units: -g=$cg_ plain=$cp_ of $(grep -c '' "$T/u18g.h")"

# --- 19 a units.txt not of spinel's ------------------------------------------------
# a directory is spinel's unit set by a units.txt spinel wrote, and even
# then not when its program.c is someone else's: both are refused without
# --force, and the program.c there is left as it was
SM=test/raise_reraise_in_def.rb
mkdir -p "$T/d19a" "$T/d19b"
echo "my notes" > "$T/d19a/units.txt"; echo "int mine;" > "$T/d19a/program.c"
cp "$T/u18p/units.txt" "$T/d19b/units.txt"; echo "int mine;" > "$T/d19b/program.c"
"$SP" "$SM" -c --incremental -o "$T/d19a" > "$T/d19a.err" 2>&1; ra=$?
"$SP" "$SM" -c --incremental -o "$T/d19b" > "$T/d19b.err" 2>&1; rb=$?
ka=0; grep -qx 'int mine;' "$T/d19a/program.c" && ka=1
kb=0; grep -qx 'int mine;' "$T/d19b/program.c" && kb=1
na=$(grep -c "^spinel: refusing to write into '$T/d19a'" "$T/d19a.err")
nb=$(grep -c "^spinel: refusing to write into '$T/d19b'" "$T/d19b.err")
o=0; [ $ra != 0 ] && [ $rb != 0 ] && [ $ka = 1 ] && [ $kb = 1 ] && [ "$na" = 1 ] && [ "$nb" = 1 ] && o=1
report "19 a units.txt not of spinel's" $o "rc=$ra/$rb refused=$na/$nb program.c-kept=$ka/$kb"

# --- 20 a unit set written over ----------------------------------------------------
# the units the previous unit set listed and this one does not are removed;
# a unit_*.c it did not list stays
cp -R "$T/u18p" "$T/d20"
echo "int notes;" > "$T/d20/unit_notes.c"
cp "$T/wb.rb" "$T/s18/prog.rb"
"$SP" "$T/s18/prog.rb" -c --incremental -o "$T/d20" > "$T/d20.err" 2>&1; r=$?
listed=$(awk 'NR>2' "$T/d20/units.txt" | grep -c '')
files=$(find "$T/d20" -name 'unit_*.c' ! -name unit_notes.c | wc -l | tr -d ' ')
kept=0; [ -f "$T/d20/unit_notes.c" ] && kept=1
o=0; [ $r = 0 ] && [ "$listed" -gt 0 ] && [ "$files" = "$listed" ] && [ $kept = 1 ] && o=1
report "20 a unit set written over" $o "rc=$r unit-files=$files listed=$listed unit_notes.c-kept=$kept"

# --- 21 -g cell barrier temps ------------------------------------------------------
# a local a block captures lives in a heap cell, and a store into the cell
# names a _wcN temp. Under -g the methods are not static, so the count ran on
# from one method to the next: four cell stores more in m1 renamed the cells
# of m2, m3 and m4 and missed their units. They are numbered from 1 again at
# each unit now, so the edit changes the units of m1 alone, as in a plain
# build (units of 1K, to split the methods)
for v in a b; do
  {
    echo "def m1"
    if [ $v = b ]; then echo "  u = nil; t = nil; u = nil; t = nil; u = nil; t = nil"; else echo "  u = nil; t = nil"; fi
    echo "  pr = proc { t = (u = \"a\" * 3).size.to_s; u }"
    echo "  pr.call"
    echo "end"
    for m in m2 m3 m4; do
      echo "def $m"
      echo "  u = nil; t = nil"
      echo "  pr = proc { t = (u = \"$m\" * 3).size.to_s; u }"
      echo "  pr.call"
      echo "end"
    done
    echo "puts m1, m2, m3, m4"
  } > "$T/c21$v.rb"
done
uset c21a u21p --unit-bytes=1K; uset c21b u21pw --unit-bytes=1K
uset c21a u21g -g --unit-bytes=1K; uset c21b u21gw -g --unit-bytes=1K
cp_=$(comm -13 "$T/u21p.h" "$T/u21pw.h" | wc -l | tr -d ' ')
cg_=$(comm -13 "$T/u21g.h" "$T/u21gw.h" | wc -l | tr -d ' ')
o=0; [ "$cp_" -ge 1 ] && [ "$cg_" -ge 1 ] && [ "$cg_" -le "$cp_" ] && o=1
report "21 -g cell barrier temps" $o "changed units: -g=$cg_ plain=$cp_ of $(grep -c '' "$T/u21g.h")"

if [ $fail = 0 ]; then echo "incremental-test: pass"; else echo "incremental-test: FAILED:$failed"; fi
exit $fail
