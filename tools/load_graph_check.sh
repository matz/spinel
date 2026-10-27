#!/bin/sh
# Under --load-model=graph (#9898) each require is a LoadNode carrying the
# file it loads: the one its own call resolved to, also where flatten meets
# a file's requires in another order than its tree has them (a begin's
# ensure before its else) and when two are on one line. The libraries
# CRuby has at boot are LoadNodes ahead of the entry's statements, in the
# order they load.
#
# Each required file's top level runs at its require, once, with locals of
# its own: every test/load_graph program with an .expected prints what
# CRuby prints and exits as CRuby does, 0 unless a `# spinel: exit N` line
# in its first lines gives N (the others are the files they require).
# A require that names no file raises LoadError when it runs, and a
# --debug build reports a required file's frames as CRuby does
# (`<top (required)>`, `Kernel#require_relative`). What graph mode does not
# compile yet it refuses by name: a require below a file's top level or one
# whose path is computed, an autoload, a load of the entry file, an END
# block, a write of main's instance variable or a top-level include in a
# required file. A required file that only defines things costs no C.
# The load record's points of a program with a .load-record are those it
# lists.
#
# Each case below is a program (`program NAME SOURCE LIB...`: main.rb from
# SOURCE, and LIB.rb for each LIB, printing its name) and what its
# --dump-ast must hold (`loads NAME BOOTS PATH=FILE...`): the entry's body
# opens with BOOTS boot LoadNodes, in the order --dump-load-graph places
# them, and the program's LoadNodes are exactly the PATH=FILE pairs, the
# path a require names and the end of the path of the file its
# target_file is in the load graph.
SPINEL=${SPINEL:-bin/spinel}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-loadgraph.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
ok=1
fail() { echo "load-graph-test: FAIL ($1)"; ok=0; }

program() {
  name=$1 src=$2; shift 2
  mkdir -p "$tmp/cases/$name"
  printf '%b' "$src" > "$tmp/cases/$name/main.rb"
  for lib in "$@"; do printf 'puts "%s"\n' "$lib" > "$tmp/cases/$name/$lib.rb"; done
}

loads() {
  name=$1 boots=$2; shift 2
  t=$tmp/cases/$name/main.rb
  if ! "$SPINEL" --load-model=graph --dump-load-graph "$t" >"$tmp/graph" 2>"$tmp/err" ||
     ! "$SPINEL" --load-model=graph --dump-ast "$t" >"$tmp/ast" 2>>"$tmp/err"; then
    fail "$name: did not flatten"; sed -n 1,3p "$tmp/err"; return
  fi
  # each LoadNode as its path and its target file
  awk 'FNR == NR { if ($1 == "file") file[$2] = $3; next }
       $1 == "N" && $3 == "LoadNode" { load[$2] = 1 }
       $1 == "S" && $3 == "path" { path[$2] = $4 }
       $1 == "I" && $3 == "target_file" { to[$2] = $4 }
       END { for (id in load) print path[id], file[to[id]] }' "$tmp/graph" "$tmp/ast" > "$tmp/pairs"
  [ "$(wc -l < "$tmp/pairs")" -eq $# ] || { fail "$name: $(wc -l < "$tmp/pairs" | tr -d ' ') LoadNodes, expected $#"; cat "$tmp/pairs"; }
  for want in "$@"; do
    awk -v p="${want%%=*}" -v f="/${want#*=}" '$1 == p && substr($2, length($2) - length(f) + 1) == f { n++ } END { exit n != 1 }' "$tmp/pairs" ||
      { fail "$name: no one LoadNode of \"${want%%=*}\" loads ${want#*=}"; cat "$tmp/pairs"; }
  done
  # the boot LoadNodes open the entry's body, the first placed at 0:-n
  awk -v want="$boots" '
       FNR == NR { if ($1 == "order") order[substr($2, 1, length($2) - 1)] = $3; next }
       $1 == "ROOT" { root = $2 }
       $1 == "R" && $2 == root && $3 == "statements" { st = $4 }
       $1 == "A" && $2 == st && $3 == "body" { n = split($4, body, ",") }
       $1 == "I" && $3 == "boot" { boot[$2] = 1 }
       $1 == "I" && $3 == "target_file" { to[$2] = $4 }
       END { for (k = 0; k < n && (body[k + 1] in boot); k++) ;
             bad = k != want
             for (i = 1; i <= k; i++) { got = order[to[body[i]]]; places = places " " got; bad = bad || got != "0:" (i - 1 - k) }
             if (bad) { print "the entry opens with " k " boot LoadNodes (expected " want "), placed at" places; exit 1 } }' \
    "$tmp/graph" "$tmp/ast" > "$tmp/boot" || fail "$name: $(cat "$tmp/boot")"
}

# requires in a begin's else and ensure, on lines of their own and on one line
program lines 'begin\n  puts "body"\nrescue\n  puts "rescue"\nelse\n  require_relative "a"\nensure\n  require_relative "b"\nend\n' a b
loads lines 0 a=a.rb b=b.rb
program one_line 'begin puts "body"; rescue; puts "rescue"; else require_relative "c"; ensure require_relative "d"; end\n' c d
loads one_line 0 c=c.rb d=d.rb

# libraries CRuby has at boot: Set, Enumerable's methods written in Ruby,
# and the enumerator a break leaves
program boot 's = Set.new([1, 2])\np s.size\np [1, 2].map { |x| break x }\n'
loads boot 3 set=packages/set/set.rb builtins/enumerable=builtins/enumerable.rb builtins/enumerator=builtins/enumerator.rb


# ---- compiled: each file's top level at its load ----
for e in test/load_graph/*.rb.expected; do
  t=${e%.expected}
  b=$(basename "$t" .rb)
  want=$(sed -n '1,5s/^# spinel: exit \([0-9][0-9]*\)$/\1/p' "$t")
  if "$SPINEL" --load-model=graph "$t" -o "$tmp/$b" >"$tmp/$b.c.out" 2>&1; then
    "$tmp/$b" >"$tmp/$b.run" 2>/dev/null
    got=$?
    cmp -s "$tmp/$b.run" "$e" ||
      { fail "$t does not print its .expected"; diff "$e" "$tmp/$b.run" | sed -n 1,10p; }
    [ "$got" -eq "${want:-0}" ] || fail "$t exits $got, not ${want:-0}"
  else
    fail "$t did not compile"; sed -n 1,5p "$tmp/$b.c.out"
  fi
done

# ---- the load record: a test/load_graph program with a .load-record dumps
# those points (--dump-load-record, analyze_elab.c) ----
for e in test/load_graph/*.rb.load-record; do
  t=${e%.load-record}
  "$SPINEL" --load-model=graph --dump-load-record "$t" >"$tmp/record" 2>"$tmp/record.err" ||
    { fail "$t: no load record"; sed -n 1,5p "$tmp/record.err"; }
  cmp -s "$tmp/record" "$e" || { fail "$t's load record is not its .load-record"; diff "$e" "$tmp/record" | sed -n 1,10p; }
done

# A raise in a required file leaves its load with the frames it was raised
# with: a --debug build's report of it names the raising method first, not
# the load the exception passes through.
mkdir "$tmp/bt"
printf 'def helper\n  raise ArgumentError, "bad"\nend\nhelper\n' > "$tmp/bt/lib.rb"
printf 'puts "before"\nrequire_relative "lib"\n' > "$tmp/bt/main.rb"
if "$SPINEL" --load-model=graph --debug --no-inline-hot "$tmp/bt/main.rb" -o "$tmp/bt/bin" >/dev/null 2>&1; then
  "$tmp/bt/bin" >/dev/null 2>"$tmp/bt/err"
  head -1 "$tmp/bt/err" | grep -q "lib.rb:\([0-9]*:\)\{0,1\}in .helper': bad (ArgumentError)" ||
    { fail "a raise in a required file lost its frames"; cat "$tmp/bt/err"; }
else
  fail "bt did not compile"
fi

# A required file's top level is CRuby's `<top (required)>` frame in its own
# file, and the load below it `Kernel#require` / `Kernel#require_relative` at
# the requiring line, through two levels: in caller(0) at a file's top
# level, in an uncaught raise's report, and in the report of the LoadError
# of a require that names no file. The runtime reads a frame's line with
# addr2line (Linux) or atos (macOS); without it a frame has no line, and a
# frame in the runtime's C cannot take its caller's file either, so then
# only the frames' names are checked.
# frames WANT GOT: each line of GOT, from the last `/` on (from `in ` on,
# names only), matches WANT's
frames() {
  if [ -n "$names_only" ]; then cut='s|^.*:in |in |'; else cut='s|^.*/||'; fi
  sed "$cut" "$1" > "$1.cut"; sed "$cut" "$2" > "$2.cut"
  paste "$1.cut" "$2.cut" | while IFS='	' read -r w g; do
    printf '%s\n' "$g" | grep -q "^$w\$" || echo "want $w, got $g"; done
}
mkdir "$tmp/frames"
printf 'puts "a"\nrequire "frames_b"\n' > "$tmp/frames/a.rb"
printf 'def helper\n  raise ArgumentError, "bad"\nend\nhere = caller(0)\nputs here\nhelper if ARGV.empty?\nrequire "frames_none"\n' > "$tmp/frames/frames_b.rb"
printf 'puts "main"\nrequire_relative "a"\n' > "$tmp/frames/main.rb"
if "$SPINEL" --load-model=graph --debug --no-inline-hot -I "$tmp/frames" "$tmp/frames/main.rb" -o "$tmp/frames/bin" >/dev/null 2>&1; then
  "$tmp/frames/bin" > "$tmp/frames/out" 2>"$tmp/frames/err"
  "$tmp/frames/bin" x >/dev/null 2>"$tmp/frames/lerr"
  n='\([0-9]*:\)\{0,1\}'; n2=$n n4=$n n6=$n n7=$n
  case "$(uname -s)" in Linux) sym=addr2line;; Darwin) sym=atos;; *) sym=;; esac
  names_only=
  if [ -n "$sym" ] && command -v "$sym" >/dev/null 2>&1; then n2=2: n4=4: n6=6: n7=7:
  elif [ -n "$sym" ]; then
    names_only=1 n2= n4= n6= n7=
    echo "load-graph-test: no $sym here, so the --debug frames' lines and files go unchecked (only their names)"
  fi
  below="a.rb:${n2}in .Kernel#require'
a.rb:${n2}in .<top (required)>'
main.rb:${n2}in .Kernel#require_relative'
main.rb:${n2}in .<main>'"
  printf '%s\n' "frames_b.rb:${n4}in .<top (required)>'" "$below" > "$tmp/frames/want"
  sed -n '3,$p' "$tmp/frames/out" > "$tmp/frames/got"
  frames "$tmp/frames/want" "$tmp/frames/got" > "$tmp/frames/bad"
  [ -s "$tmp/frames/bad" ] && { fail "caller(0) at a required file's top level"; cat "$tmp/frames/bad"; }
  printf '%s\n' "frames_b.rb:${n2}in .helper': bad (ArgumentError)" "frames_b.rb:${n6}in .<top (required)>'" "$below" > "$tmp/frames/want"
  frames "$tmp/frames/want" "$tmp/frames/err" > "$tmp/frames/bad"
  [ -s "$tmp/frames/bad" ] && { fail "an uncaught raise in a required file's method"; cat "$tmp/frames/bad"; }
  printf '%s\n' "frames_b.rb:${n7}in .Kernel#require': cannot load such file -- frames_none (LoadError)" "frames_b.rb:${n7}in .<top (required)>'" "$below" > "$tmp/frames/want"
  frames "$tmp/frames/want" "$tmp/frames/lerr" > "$tmp/frames/bad"
  [ -s "$tmp/frames/bad" ] && { fail "the LoadError of a require that names no file"; cat "$tmp/frames/bad"; }
else
  fail "frames did not compile"
fi

# refused NAME MESSAGE: $tmp/NAME/main.rb refuses with MESSAGE
refused() {
  if "$SPINEL" --load-model=graph "$tmp/$1/main.rb" -o "$tmp/$1/bin" >"$tmp/$1/out" 2>&1; then
    fail "$1 compiled"
  else
    grep -qF -- "$2" "$tmp/$1/out" || { fail "$1 refused without naming why"; sed -n 1,3p "$tmp/$1/out"; }
  fi
}
mkdir "$tmp/in_method" "$tmp/computed" "$tmp/autoload" "$tmp/entry" "$tmp/ivar" "$tmp/end" "$tmp/mixin"
printf 'puts "lib"\n' > "$tmp/in_method/lib.rb"
printf 'def setup\n  require_relative "lib"\nend\nsetup\n' > "$tmp/in_method/main.rb"
refused in_method "loads only a require that is a statement of a file's top level"
printf 'name = ARGV.first || "lib"\nrequire name\n' > "$tmp/computed/main.rb"
refused computed "loads only a require whose path is known at compile time"
printf 'module Lib; end\n' > "$tmp/autoload/lib.rb"
printf 'autoload :Lib, File.join(__dir__, "lib")\np Lib\n' > "$tmp/autoload/main.rb"
refused autoload "an autoload's file"
printf 'require_relative "main"\n' > "$tmp/entry/lib.rb"
printf 'require_relative "lib"\nputs "main"\n' > "$tmp/entry/main.rb"
refused entry "the program's own entry file"
printf '@level = 2\n' > "$tmp/ivar/lib.rb"
printf '@level = 1\nrequire_relative "lib"\np @level\n' > "$tmp/ivar/main.rb"
refused ivar "a write of main's instance variable in a required file"
printf 'END { puts "lib END" }\nputs "lib"\n' > "$tmp/end/lib.rb"
printf 'require_relative "lib"\nputs "main"\n' > "$tmp/end/main.rb"
refused end "an END block in a required file"
printf 'include Helpers\n' > "$tmp/mixin/lib.rb"
printf 'module Helpers\n  LEVEL = 1\nend\nrequire_relative "lib"\np LEVEL\n' > "$tmp/mixin/main.rb"
refused mixin "a top-level include in a required file"
# an include in a required file after a call its module's method would
# answer early: refused as in one file (refuse_mixin_after_calls reads the
# statements in load order)
mkdir "$tmp/late_include"
printf 'module Loud\n  def speak = "LOUD"\nend\nclass Dog\nend\nd = Dog.new\nbegin\n  puts d.speak\nrescue NoMethodError\n  puts "no speak yet"\nend\nclass Dog\n  include Loud\nend\n' > "$tmp/late_include/lib.rb"
printf 'require_relative "lib"\n' > "$tmp/late_include/main.rb"
refused late_include "a later statement mixes module Loud into class Dog"

# --strict-require holds in graph mode as in the inline one: a feature no
# require names is not there (CRuby's NoMethodError for winsize, refused
# here), and one a require names is.
mkdir "$tmp/gate" "$tmp/gate_req"
printf 'p $stdout.winsize.class\n' > "$tmp/gate/main.rb"
printf 'require "io/console"\np $stdout.winsize.class\n' > "$tmp/gate_req/main.rb"
"$SPINEL" --load-model=graph --strict-require "$tmp/gate/main.rb" -o "$tmp/gate/bin" >"$tmp/gate/out" 2>&1 &&
  fail "--strict-require: a feature no require names compiled"
grep -qF "winsize" "$tmp/gate/out" || { fail "--strict-require: refused without naming winsize"; sed -n 1,3p "$tmp/gate/out"; }
"$SPINEL" --load-model=graph --strict-require "$tmp/gate_req/main.rb" -o "$tmp/gate_req/bin" >"$tmp/gate_req/out" 2>&1 ||
  { fail "--strict-require: a feature a require names did not compile"; sed -n 1,3p "$tmp/gate_req/out"; }

# a class-body call in a required file of a method a later file's
# reopening aliases: refused as in one file (CRuby runs the def, 3; the
# static alias would answer 19)
mkdir "$tmp/late_alias"
printf 'class A\n  def a = 3\n  def b = 19\n  p new.a\nend\n' > "$tmp/late_alias/lib.rb"
printf 'require_relative "lib"\nclass A\n  alias_method :a, :b\nend\np A.new.a\n' > "$tmp/late_alias/main.rb"
refused late_alias "is called in the class body before an alias rebinds it"

# A required file that only defines things costs nothing: no function, no
# load, and no Symbol for the def that ends it (its value is read by nothing).
mkdir "$tmp/lean"
printf 'class Shape\n  def initialize(side) = @side = side\nend\n' > "$tmp/lean/lib.rb"
printf 'require_relative "lib"\nputs 1\n' > "$tmp/lean/main.rb"
if "$SPINEL" --load-model=graph -c "$tmp/lean/main.rb" -o "$tmp/lean/out.c" >/dev/null 2>&1; then
  ! grep -qE 'sp_file_[0-9]+_top|sp_Kernel_require|sp_sym_names' "$tmp/lean/out.c" ||
    fail "a file that only defines things costs C"
else
  fail "lean did not compile"
fi

[ $ok -eq 1 ] && echo "load-graph-test: pass" || exit 1
