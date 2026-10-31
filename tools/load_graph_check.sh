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
# compile yet it refuses by name: an autoload, a load of the entry file, an END
# block or a top-level include in a required file. A required file that
# only defines things costs no C. A require whose path is computed when it
# runs names a set of files, or any file of the program, and loads the one
# its path names (test/load_graph/computed_*).
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

# A computed path names a file as CRuby finds one: an absolute path exactly,
# `./` from the directory the program runs in, a bare feature on the load
# path (-I here) and no other file of the same name beside it.
mkdir "$tmp/exact" "$tmp/exact/root"
printf 'puts "root dup"\n' > "$tmp/exact/root/dup.rb"
printf 'puts "beside dup"\n' > "$tmp/exact/dup.rb"
cat > "$tmp/exact/main.rb" <<'RB'
def pull(path) = require(path)
def never
  require "dup"
  require_relative "dup"
end
p pull("dup")
p pull("./dup")
p pull("dup")
p pull(File.join(__dir__, "dup"))
begin
  pull("dup/dup")
rescue LoadError => e
  p e.class
end
RB
if "$SPINEL" --load-model=graph -I "$tmp/exact/root" "$tmp/exact/main.rb" -o "$tmp/exact/bin" >"$tmp/exact/cc" 2>&1; then
  (cd "$tmp/exact" && ./bin > out 2>&1) || fail "exact exits $?"
  printf 'root dup\ntrue\nbeside dup\ntrue\nfalse\nfalse\nLoadError\n' | cmp -s - "$tmp/exact/out" ||
    { fail "a computed path did not name the file CRuby finds"; cat "$tmp/exact/out"; }
else
  fail "exact did not compile"; sed -n 1,3p "$tmp/exact/cc"
fi

# A computed path that runs through a symlink names the file it points to,
# as CRuby's does: a require_relative of a symlinked file in a symlinked
# directory runs the file, and one of the symlink or an absolute path
# through the directory's link answers false once it has run.
mkdir "$tmp/link" "$tmp/link/sub"
printf 'puts "lib ran"\n' > "$tmp/link/sub/lib.rb"
ln -s lib.rb "$tmp/link/sub/alias.rb"
ln -s sub "$tmp/link/linkdir"
cat > "$tmp/link/main.rb" <<'RB'
def pull(path) = require_relative(path)
p pull("linkdir/alias")
require_relative "sub/lib"
p pull("sub/alias")
p require(File.join(__dir__, "linkdir", "lib"))
RB
if "$SPINEL" --load-model=graph "$tmp/link/main.rb" -o "$tmp/link/bin" >"$tmp/link/cc" 2>&1; then
  "$tmp/link/bin" > "$tmp/link/out" 2>&1 || fail "link exits $?"
  printf 'lib ran\ntrue\nfalse\nfalse\n' | cmp -s - "$tmp/link/out" ||
    { fail "a computed path through a symlink did not name the file it points to"; cat "$tmp/link/out"; }
else
  fail "link did not compile"; sed -n 1,3p "$tmp/link/cc"
fi

# ...also where the symlink `alias.rb` points to a file of another name
# (`impl.txt`): a computed require of the feature runs it, and one through
# a second symlink to it, an absolute path or a literal require of the
# feature answers false after, as CRuby's do.
mkdir "$tmp/link_ext" "$tmp/link_ext/inc"
printf 'puts "impl ran"\n' > "$tmp/link_ext/inc/impl.txt"
ln -s impl.txt "$tmp/link_ext/inc/alias.rb"
ln -s impl.txt "$tmp/link_ext/inc/alias2.rb"
cat > "$tmp/link_ext/main.rb" <<'RB'
def pull(path) = require(path)
def never = require("alias")
p pull("alias")
p pull("alias2")
p pull(File.join(__dir__, "inc", "alias"))
p require("alias")
RB
if "$SPINEL" --load-model=graph -I "$tmp/link_ext/inc" "$tmp/link_ext/main.rb" -o "$tmp/link_ext/bin" >"$tmp/link_ext/cc" 2>&1; then
  "$tmp/link_ext/bin" > "$tmp/link_ext/out" 2>&1 || fail "link_ext exits $?"
  printf 'impl ran\ntrue\nfalse\nfalse\nfalse\n' | cmp -s - "$tmp/link_ext/out" ||
    { fail "a computed path through a symlink to a file of another name did not name it"; cat "$tmp/link_ext/out"; }
else
  fail "link_ext did not compile"; sed -n 1,3p "$tmp/link_ext/cc"
fi

# The run-time $LOAD_PATH starts as CRuby's does: the directories from -I,
# then the library roots, so a bare computed path searches them in order and
# a directory the program adds comes after (<<, push, insert past the -I
# directories, or the same one twice) or before (unshift, insert at 0). Two
# files of one name, in the -I directory and in the one the program adds.
mkdir "$tmp/order" "$tmp/order/inc" "$tmp/order/app"
printf 'puts "inc"\n' > "$tmp/order/inc/dup.rb"
printf 'puts "app"\n' > "$tmp/order/app/dup.rb"
order() {   # NAME STATEMENT EXPECTED-OUTPUT
  mkdir "$tmp/order/$1"
  printf 'if ARGV.size > 5\n  require_relative "../inc/dup"\n  require_relative "../app/dup"\nend\n%s\ndef load_it(n) = require(n)\np load_it("dup")\np load_it("dup")\n' \
    "$2" > "$tmp/order/$1/main.rb"
  if "$SPINEL" --load-model=graph -I "$tmp/order/inc" "$tmp/order/$1/main.rb" -o "$tmp/order/$1/bin" >"$tmp/order/$1/cc" 2>&1; then
    "$tmp/order/$1/bin" > "$tmp/order/$1/out" 2>&1 || fail "order $1 exits $?"
    printf '%s\n' "$3" | cmp -s - "$tmp/order/$1/out" || { fail "order $1: \$LOAD_PATH did not search as CRuby's does"; cat "$tmp/order/$1/out"; }
  else
    fail "order $1 did not compile"; sed -n 1,3p "$tmp/order/$1/cc"
  fi
}
app='File.join(__dir__, "..", "app")'
order append "\$LOAD_PATH << $app" 'inc
true
false'
order push "\$LOAD_PATH.push $app" 'inc
true
false'
order twice "\$LOAD_PATH << $app
\$LOAD_PATH << $app" 'inc
true
false'
order unshift "\$LOAD_PATH.unshift $app" 'app
true
false'
order insert0 "\$LOAD_PATH.insert(0, $app)" 'app
true
false'
order insert1 "\$LOAD_PATH.insert(1, $app)" 'inc
true
false'

# A literal require searches the load path in the same order, at compile
# time: a directory the program puts in front (unshift, prepend) comes
# before the -I directories, also one that is an -I directory already, and
# the last one put in front first; one it appends comes after them. The
# computed require after it names the file the literal one loaded.
mkdir "$tmp/order/app2"
printf 'puts "app2"\n' > "$tmp/order/app2/dup.rb"
lorder() {   # NAME STATEMENTS EXPECTED-FIRST-LINE -I-DIR...
  lo_name=$1 lo_stmts=$2 lo_want=$3; shift 3
  mkdir "$tmp/order/$lo_name"
  printf '%s\nrequire "dup"\ndef load_it(n) = require(n)\np load_it("dup")\n' "$lo_stmts" > "$tmp/order/$lo_name/main.rb"
  for d in "$@"; do set -- "$@" -I "$tmp/order/$d"; shift; done   # (each -I and its directory one word)
  if "$SPINEL" --load-model=graph "$@" "$tmp/order/$lo_name/main.rb" -o "$tmp/order/$lo_name/bin" >"$tmp/order/$lo_name/cc" 2>&1; then
    "$tmp/order/$lo_name/bin" > "$tmp/order/$lo_name/out" 2>&1 || fail "order $lo_name exits $?"
    printf '%s\nfalse\n' "$lo_want" | cmp -s - "$tmp/order/$lo_name/out" ||
      { fail "order $lo_name: a literal require did not search as CRuby's does"; cat "$tmp/order/$lo_name/out"; }
  else
    fail "order $lo_name did not compile"; sed -n 1,3p "$tmp/order/$lo_name/cc"
  fi
}
app2='File.join(__dir__, "..", "app2")'
lorder lit_unshift "\$LOAD_PATH.unshift $app" app inc
lorder lit_prepend "\$LOAD_PATH.prepend $app" app inc
lorder lit_append "\$LOAD_PATH << $app" inc inc
lorder lit_unshift_inc "\$LOAD_PATH.unshift $app" app inc app
lorder lit_append_unshift "\$LOAD_PATH << $app2
\$LOAD_PATH.unshift $app" app inc
lorder lit_unshift_twice "\$LOAD_PATH.unshift $app
\$LOAD_PATH.unshift $app2" app2 inc

# A symlinked -I directory is in $LOAD_PATH as written (File.expand_path
# does not resolve it) and still names the program's file; the feature root
# after the -I directories is a directory that exists.
mkdir "$tmp/order/sym"
ln -s "$tmp/order/inc" "$tmp/order/sym/lnk"
printf 'if ARGV.size > 5\n  require_relative "../inc/dup"\nend\np $LOAD_PATH[0].end_with?("/sym/lnk")\np File.directory?($LOAD_PATH[1])\ndef load_it(n) = require(n)\np load_it("dup")\n' > "$tmp/order/sym/main.rb"
if "$SPINEL" --load-model=graph -I "$tmp/order/sym/lnk" "$tmp/order/sym/main.rb" -o "$tmp/order/sym/bin" >"$tmp/order/sym/cc" 2>&1; then
  "$tmp/order/sym/bin" > "$tmp/order/sym/out" 2>&1 || fail "sym exits $?"
  printf 'true\ntrue\ninc\ntrue\n' | cmp -s - "$tmp/order/sym/out" ||
    { fail "a symlinked -I directory was not on \$LOAD_PATH as written, or did not name its file"; cat "$tmp/order/sym/out"; }
else
  fail "sym did not compile"; sed -n 1,3p "$tmp/order/sym/cc"
fi

# A library (--ext-init) runs its top level in its init function, not in
# main: the init starts $LOAD_PATH as main does, so a computed path there
# searches the -I directories too. A pure-C host runs the init, as
# `make ext-test`'s do.
mkdir "$tmp/ext" "$tmp/ext/inc"
printf 'puts "xlib"\n' > "$tmp/ext/inc/xlib.rb"
printf 'def never = require("xlib")\nname = ARGV.empty? ? "xlib" : ARGV.first\np require(name)\n' > "$tmp/ext/main.rb"
printf 'void spx_lp(void);\nint main(void) { spx_lp(); return 0; }\n' > "$tmp/ext/host.c"
rt=$(dirname "$SPINEL")/../lib
if "$SPINEL" --load-model=graph -I "$tmp/ext/inc" -c --no-line-map --ext-init spx_lp "$tmp/ext/main.rb" -o "$tmp/ext/out.c" >"$tmp/ext/cc" 2>&1 &&
   ${CC:-cc} -O1 -w -I"$rt" "$tmp/ext/host.c" "$tmp/ext/out.c" "$rt/libspinel_rt.a" -lm -o "$tmp/ext/host" >>"$tmp/ext/cc" 2>&1; then
  "$tmp/ext/host" > "$tmp/ext/out" 2>&1 || fail "ext host exits $?"
  printf 'xlib\ntrue\n' | cmp -s - "$tmp/ext/out" ||
    { fail "a library's top level did not find a file on its -I directory"; cat "$tmp/ext/out"; }
else
  fail "ext did not build"; sed -n 1,3p "$tmp/ext/cc"
fi

# A file beside the program is on its load path only where the program puts
# its directory there ($LOAD_PATH.unshift __dir__). A literal require finds
# it after a statement of a top level that does, and a computed one finds it
# whenever the program's $LOAD_PATH holds it as the require runs; either is
# a LoadError before, as CRuby has it.
mkdir "$tmp/lp_no" "$tmp/lp_yes" "$tmp/lp_before" "$tmp/lp_cond" "$tmp/lp_cbefore" "$tmp/lp_ccond"
for d in lp_no lp_yes lp_before lp_cond lp_cbefore lp_ccond; do printf 'puts "x loaded"\n' > "$tmp/$d/x.rb"; done
printf 'def pull(path) = require(path)\ndef never = require_relative("x")\nbegin\n  pull("x")\nrescue LoadError => e\n  p e.class\nend\n' > "$tmp/lp_no/main.rb"
printf '$LOAD_PATH.unshift __dir__\ndef pull(path) = require(path)\ndef never = require_relative("x")\np pull("x")\np pull("x")\np require("x")\n' > "$tmp/lp_yes/main.rb"
printf 'def never = require_relative("x")\nbegin\n  require "x"\nrescue LoadError => e\n  p e.class\nend\n$LOAD_PATH.unshift __dir__\np require("x")\n' > "$tmp/lp_before/main.rb"
printf '$LOAD_PATH.unshift __dir__ if false\ndef never = require_relative("x")\nbegin\n  require "x"\nrescue LoadError => e\n  p e.class\nend\n' > "$tmp/lp_cond/main.rb"
printf 'def pull(path) = require(path)\ndef never = require_relative("x")\nbegin\n  pull("x")\nrescue LoadError => e\n  p e.class\nend\n$LOAD_PATH.unshift __dir__\np pull("x")\n' > "$tmp/lp_cbefore/main.rb"
printf '$LOAD_PATH.unshift __dir__ if ARGV.empty?\ndef pull(path) = require(path)\ndef never = require_relative("x")\np pull("x")\n' > "$tmp/lp_ccond/main.rb"
for d in lp_no lp_yes lp_before lp_cond lp_cbefore lp_ccond; do
  if "$SPINEL" --load-model=graph "$tmp/$d/main.rb" -o "$tmp/$d/bin" >"$tmp/$d/cc" 2>&1; then "$tmp/$d/bin" > "$tmp/$d/out" 2>&1 || fail "$d exits $?"
  else fail "$d did not compile"; sed -n 1,3p "$tmp/$d/cc"; fi
done
printf 'LoadError\n' | cmp -s - "$tmp/lp_no/out" || { fail "a bare computed path found a file beside the program, which is not on the load path"; cat "$tmp/lp_no/out"; }
printf 'x loaded\ntrue\nfalse\nfalse\n' | cmp -s - "$tmp/lp_yes/out" || { fail "a bare computed path did not find a file on the load path the program wrote"; cat "$tmp/lp_yes/out"; }
printf 'LoadError\nx loaded\ntrue\n' | cmp -s - "$tmp/lp_before/out" || { fail "a require found a directory the program put on its load path after it"; cat "$tmp/lp_before/out"; }
printf 'LoadError\nx loaded\ntrue\n' | cmp -s - "$tmp/lp_cbefore/out" || { fail "a computed path found a directory the program put on its load path after it ran"; cat "$tmp/lp_cbefore/out"; }
printf 'x loaded\ntrue\n' | cmp -s - "$tmp/lp_ccond/out" || { fail "a computed path missed a directory a conditional statement put on the load path when it ran"; cat "$tmp/lp_ccond/out"; }
printf 'LoadError\n' | cmp -s - "$tmp/lp_cond/out" || { fail "a require found a directory a conditional statement may have put on the load path"; cat "$tmp/lp_cond/out"; }

# refused NAME MESSAGE: $tmp/NAME/main.rb refuses with MESSAGE
refused() {
  if "$SPINEL" --load-model=graph "$tmp/$1/main.rb" -o "$tmp/$1/bin" >"$tmp/$1/out" 2>&1; then
    fail "$1 compiled"
  else
    grep -qF -- "$2" "$tmp/$1/out" || { fail "$1 refused without naming why"; sed -n 1,3p "$tmp/$1/out"; }
  fi
}
mkdir "$tmp/autoload" "$tmp/entry" "$tmp/end" "$tmp/mixin"
printf 'module Lib; end\n' > "$tmp/autoload/lib.rb"
printf 'autoload :Lib, File.join(__dir__, "lib")\np Lib\n' > "$tmp/autoload/main.rb"
# A possible autoload definition anywhere keeps the original graph refusal.
# Inline keeps its eager rewrite, even when the program owns the name.
printf 'puts "must not load"\nclass Lib; end\n' > "$tmp/autoload/lib.rb"
cat > "$tmp/autoload/main.rb" <<'RUBY'
def autoload(*) = puts("user top")
autoload(:Lib, File.join(__dir__, "lib"))
RUBY
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
if "$SPINEL" --load-model=inline "$tmp/autoload/main.rb" -o "$tmp/autoload/bin" >"$tmp/autoload/cc" 2>&1; then
  "$tmp/autoload/bin" > "$tmp/autoload/run"
  printf 'must not load\n' | cmp -s - "$tmp/autoload/run" || fail "autoload inline changed its eager output"
else
  fail "autoload inline did not compile"
fi
cat > "$tmp/autoload/main.rb" <<'RUBY'
class Holder
  def x(*) = puts("user alias")
  alias_method :autoload, :x
  def run
    autoload(:Lib, File.join(__dir__, "lib"))
  end
end
Holder.new.run
RUBY
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
if "$SPINEL" --load-model=inline "$tmp/autoload/main.rb" -o "$tmp/autoload/bin" >"$tmp/autoload/cc" 2>&1; then
  "$tmp/autoload/bin" > "$tmp/autoload/run"
  printf 'must not load\n' | cmp -s - "$tmp/autoload/run" || fail "autoload inline changed its eager output"
else
  fail "autoload inline did not compile"
fi
printf 'puts "loaded"\nclass Holder; Z = 7; end\n' > "$tmp/autoload/lib.rb"
cat > "$tmp/autoload/main.rb" <<'RUBY'
class Other
  def autoload(*) = puts("user other")
end
class Holder
  autoload :Z, File.join(__dir__, "lib")
end
puts "before"
p Holder::Z
RUBY
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
if "$SPINEL" --load-model=inline "$tmp/autoload/main.rb" -o "$tmp/autoload/bin" >"$tmp/autoload/cc" 2>&1; then
  "$tmp/autoload/bin" > "$tmp/autoload/run"
  printf 'loaded\nbefore\n7\n' | cmp -s - "$tmp/autoload/run" || fail "autoload inline changed its eager output"
else
  fail "autoload inline did not compile"
fi
# A scoped read through a class whose superclass chain the record does not
# know may reach Object's registration: refused as on master.
printf 'puts "loaded"\nclass Scoped; end\n' > "$tmp/autoload/lib.rb"
cat > "$tmp/autoload/main.rb" <<'RUBY'
Base = (ARGV.size > 5 ? Class.new(BasicObject) : Object)
class Holder < Base; end
autoload :Scoped, File.join(__dir__, "lib")
begin
  Holder::Scoped
rescue NameError
  puts "ne"
end
RUBY
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
# A top-level include of an autoloaded module, a scoped read through a builtin
# class, and a query of the constant table beside a registration are refused.
printf 'puts "loaded"\nmodule Lib; def hi = 1; end\n' > "$tmp/autoload/lib.rb"
printf 'autoload :Lib, File.join(__dir__, "lib")\ninclude Lib\np hi\n' > "$tmp/autoload/main.rb"
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
# ... and so are a conditional include and a send of include in a class body.
for form in 'include Lib if ARGV.empty?' 'send(:include, Lib)'; do
  printf 'autoload :Lib, File.join(__dir__, "lib")\nclass Holder\n  %s\nend\np Holder.new.hi\n' "$form" > "$tmp/autoload/main.rb"
  refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
done
printf 'autoload :Lib, File.join(__dir__, "lib")\nbegin\n  String::Lib\nrescue NameError\n  puts "ne"\nend\n' > "$tmp/autoload/main.rb"
refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
# A feature name with no directory is looked up on the load path, not beside the file.
printf 'autoload :Lib, "lib"\np Lib\n' > "$tmp/autoload/main.rb"
refused autoload "unresolved autoload registration"
for form in 'p Object.autoload?(:Lib)' 'p Object.constants'; do
  printf 'autoload :Lib, File.join(__dir__, "lib")\n%s\n' "$form" > "$tmp/autoload/main.rb"
  refused autoload "--load-model=graph: constant-table query with autoload registrations, for now"
done
printf 'module Lib; end\n' > "$tmp/autoload/lib.rb"
for form in 'define_method(["other"].first.to_sym) {}' 'def old; end; alias :"#{:other}" old'; do
  printf 'class Holder\n  %s\n  autoload :Lib, File.join(__dir__, "lib")\nend\n' "$form" > "$tmp/autoload/main.rb"
  refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
done
printf 'autoload :Lib, File.join(__dir__, "lib")\np Lib\n' > "$tmp/autoload/main.rb"
# Closed autoload registrations compile; unresolved names and path sets are refused.
"$SPINEL" --load-model=graph "$tmp/autoload/main.rb" -o "$tmp/autoload/bin" >"$tmp/autoload/out" 2>&1 || fail "autoload did not compile"
printf 'autoload ARGV.first, File.join(__dir__, "lib")\np Lib\n' > "$tmp/autoload/main.rb"
refused autoload "unresolved autoload registration"
printf 'autoload :Lib, ARGV.first\np Lib\n' > "$tmp/autoload/main.rb"
refused autoload "unresolved autoload registration"
printf '%%w[lib other].each { |path| autoload :Lib, File.join(__dir__, path) }\np Lib\n' > "$tmp/autoload/main.rb"
refused autoload "unresolved autoload registration"
# An implicit rescue/ensure wraps the whole body, which its emitter cannot
# dispatch. Keep the graph refusal; an explicit begin statement is executable.
for body in 'class Holder' 'module Holder' 'class Holder; end; class << Holder'; do
  for clause in rescue ensure; do
    printf '%s\n  autoload :Lib, File.join(__dir__, "lib")\n%s\n  puts "clause"\nend\n' "$body" "$clause" > "$tmp/autoload/main.rb"
    refused autoload "--load-model=graph loads an autoload's file at its constant's first reference only later, for now"
  done
done
printf 'require_relative "main"\n' > "$tmp/entry/lib.rb"
printf 'require_relative "lib"\nputs "main"\n' > "$tmp/entry/main.rb"
refused entry "the program's own entry file"
printf '%%w[ig ih].each_with_index { |v, i| require File.join("main", v) }\n' > "$tmp/entry/main.rb"
refused entry "a computed require may load the program's own entry file"
printf 'Object.new.autoload(:Lib, File.join(__dir__, "lib"))\n' > "$tmp/autoload/main.rb"
refused autoload "unresolved autoload owner"
printf 'END { puts "lib END" }\nputs "lib"\n' > "$tmp/end/lib.rb"
printf 'require_relative "lib"\nputs "main"\n' > "$tmp/end/main.rb"
refused end "an END block in a required file"
printf 'include Helpers\n' > "$tmp/mixin/lib.rb"
printf 'module Helpers\n  LEVEL = 1\nend\nrequire_relative "lib"\np LEVEL\n' > "$tmp/mixin/main.rb"
refused mixin "a top-level include in a required file"
# an include in a required file after a call its module's method would
# answer early: the call reaches what the class had then, as in one file
# (the include's copies are guarded until its statement has run; the
# statements are read in load order)
mkdir "$tmp/late_include"
printf 'module Loud\n  def speak = "LOUD"\nend\nclass Dog\nend\nd = Dog.new\nbegin\n  puts d.speak\nrescue NoMethodError\n  puts "no speak yet"\nend\nclass Dog\n  include Loud\nend\n' > "$tmp/late_include/lib.rb"
printf 'require_relative "lib"\n' > "$tmp/late_include/main.rb"
# a file a method requires, once that method may run ahead of the include,
# as a call in its body may
mkdir "$tmp/late_include_method"
printf 'p C.new.tag\n' > "$tmp/late_include_method/lib.rb"
printf 'module M\n  def tag = "m"\nend\nclass C\nend\ndef load_plugin\n  require_relative "lib"\nend\nbegin\n  load_plugin\nrescue NoMethodError\n  puts "no tag yet"\nend\nclass C\n  include M\nend\n' > "$tmp/late_include_method/main.rb"
for d in late_include late_include_method; do
  if "$SPINEL" --load-model=graph "$tmp/$d/main.rb" -o "$tmp/$d/bin" >"$tmp/$d/cc" 2>&1; then
    "$tmp/$d/bin" >"$tmp/$d/out" 2>&1 || fail "$d exits $?"
    case $d in late_include) want='no speak yet' ;; *) want='no tag yet' ;; esac
    printf '%s\n' "$want" | cmp -s - "$tmp/$d/out" || { fail "$d called the module's method before its include ran"; cat "$tmp/$d/out"; }
  else
    fail "$d did not compile"; sed -n 1,3p "$tmp/$d/cc"
  fi
done

# A require whose path no expression tells may name any file of the program,
# so a file it might run first has no fixed place: a file whose require is
# under a condition, one that comes after the method holding the computed
# require, and one a file with no place requires. A file whose require
# precedes the computed one, as a statement of a top level, is placed
# (computed_open_main). An alias with one target version in an unplaced file
# answers without an order comparison; multiple versions still require an
# order and are refused.
mkdir "$tmp/open_cond" "$tmp/open_after" "$tmp/open_nested"
open_lib() { printf 'class OpenMoney\n  def initialize(c) = @c = c\n  def +(o) = OpenMoney.new(@c + o)\n  def cents = @c\nend\n' > "$tmp/$1/lib.rb"; }
open_lib open_cond
printf 'require_relative "lib" if ARGV.empty?\ndef pull(path) = require_relative(path)\nclass OpenMoney\n  alias_method :add, :+\nend\np pull("lib")\np OpenMoney.new(10).add(1).cents\n' > "$tmp/open_cond/main.rb"
open_lib open_after
printf 'def pull(path) = require_relative(path)\nrequire_relative "lib"\nclass OpenMoney\n  alias_method :add, :+\nend\np pull("lib")\np OpenMoney.new(10).add(1).cents\n' > "$tmp/open_after/main.rb"
open_lib open_nested
printf 'require_relative "lib"\n' > "$tmp/open_nested/wrap.rb"
printf 'def setup = require_relative("wrap")\nsetup\ndef pull(path) = require_relative(path)\nclass OpenMoney\n  alias_method :add, :+\nend\np pull("lib")\np OpenMoney.new(10).add(1).cents\n' > "$tmp/open_nested/main.rb"

for d in open_cond open_after open_nested; do
  if "$SPINEL" --load-model=graph "$tmp/$d/main.rb" -o "$tmp/$d/bin" >"$tmp/$d/cc" 2>&1; then
    "$tmp/$d/bin" >"$tmp/$d/out" 2>&1 || fail "$d exits $?"
    printf 'false\n11\n' | cmp -s - "$tmp/$d/out" || { fail "$d did not keep the current alias target"; cat "$tmp/$d/out"; }
  else
    fail "$d did not compile"; sed -n 1,3p "$tmp/$d/cc"
  fi
done

# Two versions of the target still need an order: the alias captures the
# first body even though the method loaded later replaces the target name.
mkdir "$tmp/alias_versions"
printf 'class Timer\n  def delay=(n); n + 2; end\nend\n' > "$tmp/alias_versions/lib.rb"
printf 'def boot = require_relative("lib")\nclass Timer\n  def delay=(n); n + 1; end\n  alias_method :delay, :delay=\nend\nboot\np Timer.new.delay(10)\n' > "$tmp/alias_versions/main.rb"
refused alias_versions "the alias reading reads the order of two files"

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

# A file's loaded flag and function are declared ahead of every body, since a
# load may stand in a method or a proc defined before the file's own code.
mkdir "$tmp/decls"
printf 'puts "lib"\n' > "$tmp/decls/lib.rb"
printf 'def run = 1\nrequire_relative "lib"\np run\n' > "$tmp/decls/main.rb"
if "$SPINEL" --load-model=graph -S --no-line-map "$tmp/decls/main.rb" > "$tmp/decls/out.c" 2>/dev/null; then
  awk '/sp_file_1_top\(void\);/ && !d { d = NR } /sp_run\(void\) \{/ && !r { r = NR } END { exit !(d && r && d < r) }' "$tmp/decls/out.c" ||
    fail "a file's function is declared after the method bodies"
else
  fail "decls did not compile"
fi

# g_require_unread (spinel_parse.c): may the program load a file the compiler
# did not read? The load, autoload and load-path nodes of the trees set it, a
# require that names no file does, and the word in a comment or a String
# sets nothing. A multiple assignment into an Array holds its new Strings as
# their handles only where the flag is clear, so the C says which way a
# program was read: with it clear the second new String is held too (a line
# makes a String and holds it), with it set one is. Both load models answer
# alike. An alias of the load path or of the loaded features
# (`alias $lp $LOAD_PATH`, either way round) sets the flag the same way, but
# is refused before any C is made, so no case can show it.
held() { "$SPINEL" $2 "$1" -S --no-line-map 2>/dev/null | grep -c 'sp_String_new_shared(.*SP_GC_ENTRY_PTR'; }
unread_flag() {   # FILE HELD WHAT
  for m in inline graph; do
    got=$(held "$1" --load-model=$m)
    [ "$got" = "$2" ] || fail "$3, --load-model=$m: $got Strings held, expected $2"
  done
}
unread_flag test/load_word_in_text.rb 2 "load in a comment and in a String"
unread_flag test/load_word_called.rb 1 "a call of load and the load path"
core='r = [+"q"]\nr[0], r[1] = +"ab", +"cd"\np r\n'
unread_form() {   # NAME HELD SOURCE-BEFORE-THE-CORE
  printf '%b%b' "$3" "$core" > "$tmp/unread_$1.rb"
  unread_flag "$tmp/unread_$1.rb" "$2" "$1"
}
unread_form plain 2 ''
unread_form comment 2 '# autoload :X, "x"; $LOAD_PATH << "d"; $:; $"\n'
unread_form string 2 'p "load $LOAD_PATH #$0 autoload $-I"\n'
unread_form defname 2 'def reload_all = 1\ndef loader = 2\n'
unread_form call 1 'def load(x) = x\nload "x"\n'
unread_form method_call 1 'class K; def self.autoload(x) = x; end\nK.autoload "x"\n'
unread_form load_path 1 '$LOAD_PATH << "d"\n'
unread_form colon 1 'p $:.size\n'
unread_form dash_i 1 'p $-I.size\n'
unread_form features 1 'p $LOADED_FEATURES.size\n'
unread_form quote 1 'p $".size\n'
unread_form interpolated 1 'p "#$:"\n'
unread_form missing_require 1 'begin; require "no_such_file_for_load_bit"; rescue LoadError; end\n'

# The same flag counts the require and require_relative calls of the trees
# against the ones resolved: a require word in a comment, a String, a Symbol,
# a hash key or a def name is no call and counts for nothing, a call of
# another receiver or of a file that is not there leaves the program read as
# one that may load an unread file, and one resolved does not.
unread_flag test/require_word_in_text.rb 2 "require in a comment, a String and a Symbol"
unread_flag test/require_word_missing.rb 1 "a require of a missing file"
unread_form require_comment 2 '# require "foo"; require_relative "bar"; Kernel.require "baz"\n'
unread_form require_string 2 'p "require \x27x\x27 require_relative"\n'
unread_form require_symbol 2 'p :require, :require_relative\n'
unread_form require_key 2 'p({ require: 1 })\n'
unread_form require_def 2 'def require(x) = x\ndef require_all = 1\n'
unread_form require_resolved 2 'require "tmpdir"\n'
unread_form require_receiver 1 'class K; def self.require(x) = x; end\nK.require "x"\n'

[ $ok -eq 1 ] && echo "load-graph-test: pass" || exit 1
