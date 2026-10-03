# spin -- the Spinel project tool (M0: new/init/build/run/test/clean,
# path dependencies only, no network, no lockfile). Usage: docs/spin.md;
# design record: docs/internals/spin.md.

require_relative "spin/toml"
require "spin_version"   # build/spin_version.rb: the release and revision this spin was built from

$spin_hasher = ""   # memoized content-hasher command (native_hasher)
$spin_verbose = ENV["SPIN_VERBOSE"].to_s != ""   # --verbose / SPIN_VERBOSE=1
# newline-packed paths of .c files native_sources recognised as spinel's own
# output and left out of the build; reported once per package (#4362)
$spin_emitted_c = ""
# newline-packed paths of .c files in the package tree that `sources` does not
# name, and of `sources` entries that matched nothing; both are reported once
# per package (#4362)
$spin_undeclared_c = ""
$spin_empty_source_glob = ""
                                                  # makes run_command print the
                                                  # command before exec'ing it

# Wrap one external command. Returns true on exit 0, false on non-zero
# exit, raises on exec failure (Errno::ENOENT, etc.). Every external
# exec in this file goes through here so a single flag -- or env var --
# turns on command echoing, and so a future refactor (capture output,
# parallelize, dry-run) has one site to touch.
#
# Implementation: Process.spawn("/bin/sh", "-c", cmd) plus a
# Process.waitpid2 for the exit status. The shell wrapper handles the
# multi-token cmd string (paths, flags) without us writing a quote
# parser, and is portable: CRuby's `system(cmd)` runs the same command
# via /bin/sh, and spinel's AOT runtime routes the codegen-emitted
# spawn through its own fork/exec with the same shell semantics.
#
# When called with `text:` the child's stderr is also accumulated into
# the given String (line by line, on a background thread that writes
# each line to $stderr so it still streams live to the operator). This
# is what compile()'s "cannot load such file" diagnostic needs: the
# live stream from run_command, plus the captured text for the hint
# check afterwards. Other call sites pass nothing and get the plain
# status.
def run_command(cmd, text: nil)
  $stderr.puts cmd if $spin_verbose
  if text.nil?
    pid = Process.spawn("/bin/sh", "-c", cmd)
    _, status = Process.waitpid2(pid)
    return status.success?
  end
  rd, wr = IO.pipe
  pid = Process.spawn("/bin/sh", "-c", cmd, :err => wr)
  wr.close
  cap = Thread.new do
    while (line = rd.gets)
      $stderr.write line
      text << line
    end
  end
  _, status = Process.waitpid2(pid)
  rd.close
  cap.join
  status.success?
end

SPIN_USAGE = <<USAGE
usage: spin <command> [args]
  new <name> [--lib]   scaffold an application (or a library with --lib)
  init                 write a spin.toml into the current directory
  add <name> [--version C | --git URL [--ref R] | --path DIR]
                       [--features A,B]  add + lock
  search [term]        find packages in the index (name, latest, repo)
  remove <name>        drop a dependency + relock
  lock | fetch | vendor  resolve deps / warm the cache / copy into vendor/
  build [target..]     build bin/ executables into build/bin/
                       (--debug / -g: debuggable -O0 build for lldb/gdb;
                        also SPIN_DEBUG=1; applies to build/run/test)
  run [target] [-- a]  build, then run one executable
  test [file..]        build and run test/*.rb against expectations
  trust <name>         always allow <name>'s declared native build steps
                       (one run: --allow-native-build / SPIN_ALLOW_NATIVE_BUILD=1)
  clean                remove build/
  flags                print the compiler flags this project implies, for a
                       build driven from outside spin (Makefile, script)
  flags --deps         print the toolchain files that build, to list as
                       prerequisites: without them a rule is "up to date"
                       after the compiler or its runtime changed
  ext new <name>       scaffold a CRuby extension gem (Ruby kernel -> .so)
  ext build            emit the kernel C + shim into ext/ and vendor the runtime
  ext test             differential: each case through the pure AND compiled path
  list [--json]        resolved dependency set (name, version, source)
  tree [--json]        dependency tree from this package
  publish [--direct]   validate + test, then submit this release to the index
  install [name..]     build and copy bin/ executables to ~/.local/bin
                       (--prefix DIR, --uninstall)
  pack [name] [--out DIR]
                       write a directory that builds the program from C
                       alone (generated C, runtime, native packages, Makefile)
  <command> --help     this text; no command runs
USAGE

def spin_die(msg)
  $stderr.puts "spin: #{msg}"
  exit 1
end

# --- project discovery -------------------------------------------------------

def find_root(dir)
  d = dir
  while true
    return d if File.exist?(File.join(d, "spin.toml"))
    up = File.expand_path("..", d)
    return "" if up == d
    d = up
  end
end

# `command -v` for one name, without a shell: the PATH entry that would run.
# mkdir -p for one path, without a shell.
def mkdir_p_path(path)
  acc = path.start_with?("/") ? "/" : ""
  path.split("/").each do |seg|
    next if seg == ""
    acc = acc == "/" ? "/" + seg : (acc == "" ? seg : File.join(acc, seg))
    Dir.mkdir(acc) unless Dir.exist?(acc)
  end
  nil
end

# Searches PATH the way the shell does, so the answer is the file the shell
# ran. An empty component -- a leading or trailing ":", or "::" -- names the
# current directory. The search used to skip it, so with PATH=":/usr/local/bin"
# the shell ran ./spin while which("spin") answered /usr/local/bin/spin, and
# spinel_bin took the compiler beside the wrong spin. The -1 keeps a trailing
# empty component, which split drops otherwise.
def which(name)
  ENV["PATH"].to_s.split(":", -1).each do |dir|
    dir = "." if dir == ""
    cand = File.join(dir, name)
    return cand if File.file?(cand) && File.executable?(cand)
  end
  ""
end

def spinel_bin
  # spin ships beside the compiler: <dir-of-$0>/spinel. It has to be an
  # executable FILE: a checkout of the compiler repo cloned next to the project
  # is a DIRECTORY of that name, and File.exist? said yes to it -- spin then
  # tried to run the directory ("Permission denied", or "is a directory" on
  # macOS) instead of falling through to the installed binary (#3407).
  #
  # Run through PATH, $0 is the bare name "spin" with no directory, and
  # expand_path read it as a file in the current directory: the sibling was
  # never found, and the "spinel" fallback below is a name, not a file, so
  # File.exist?/File.mtime on it answered nothing. The freshness bound of
  # `spin test` (#3202) then left the compiler out, and after a compiler
  # upgrade every test binary the old compiler built was reused as "(cached)":
  # a green run that tested nothing new. Look $0 up on PATH as the shell did.
  me = $0
  if !me.include?("/")
    found = which(me)
    me = found if found != ""
  end
  me = File.expand_path(me)
  cand = File.join(File.expand_path("..", me), "spinel")
  if File.file?(cand) && File.executable?(cand)
    # Realpath: a symlinked install (e.g. /home/user/bin/spin ->
    # /home/user/spinel/bin/spin) has $0 resolve to the symlink path; without
    # realpath, spinel_hdr_dir's <bin>/../lib probe walks from the symlink's
    # parent and misses the install tree's lib/.
    return File.symlink?(cand) ? File.realpath(cand) : cand
  end
  "spinel"  # PATH fallback
end

# the compiler build revision -- the parenthesised word of
# "spinel <release> (<sha>) [...]" -- "" when unknown; keys the R8 probe
# records, since two builds of one release differ only there
def spinel_rev
  f = sh_read(spinel_bin + " --version").split(" ")
  r = f.length >= 3 ? f[2] : ""
  r = r[1, r.length - 2] if r.length >= 2 && r[0] == "(" && r[r.length - 1] == ")"
  r == "unknown" ? "" : r
end


# --- index (M3) ----------------------------------------------------------------
# The index is a git repo, not a server (R5): packages/<name>.toml maps a name
# its repo plus [[release]] version/ref entries. Selection is MVS: the LOWEST
# release satisfying every constraint gathered for the package, so a build without
# a lock is still deterministic; spin.lock then pins the outcome.

def spin_index_url
  u = ENV["SPIN_INDEX"].to_s
  u == "" ? "https://github.com/matz/spin-index" : u
end

def index_dir(offline)
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "index")
  return d if Dir.exist?(File.join(d, ".git"))
  spin_die("--offline: no cached index (spin fetch first)") if offline
  ok = system("git clone -q --depth 1 #{spin_index_url} #{d}")
  spin_die("cannot clone the index: " + spin_index_url) unless ok
  d
end

def index_refresh(offline)
  return if offline
  d = index_dir(false)
  system("git -C #{d} pull -q --ff-only 2>/dev/null")  # offline-tolerant: stale is usable
end

# "1.2.3" <=> "1.10" as numeric components; missing parts are 0
def vcmp(a, b)
  pa = a.split(".")
  pb = b.split(".")
  n = pa.length > pb.length ? pa.length : pb.length
  i = 0
  while i < n
    x = i < pa.length ? pa[i].to_i : 0
    y = i < pb.length ? pb[i].to_i : 0
    return -1 if x < y
    return 1 if x > y
    i += 1
  end
  0
end

# constraint: "*"/"" (any), "~> X.Y(.Z)" (pessimistic), ">= X", or exact "X.Y.Z"
def version_satisfies(v, cons)
  c = cons.strip
  return true if c == "" || c == "*"
  if c.start_with?("~>")
    floor = c[2..-1].to_s.strip
    return false if vcmp(v, floor) < 0
    parts = floor.split(".")
    return true if parts.length < 2
    ceil = ""
    i = 0
    while i < parts.length - 1
      ceil += "." unless ceil == ""
      ceil += (i == parts.length - 2 ? (parts[i].to_i + 1).to_s : parts[i])
      i += 1
    end
    return vcmp(v, ceil) < 0
  end
  return vcmp(v, c[2..-1].to_s.strip) >= 0 if c.start_with?(">=")
  vcmp(v, c) == 0
end

# MVS pick from packages/<name>.toml: lowest release satisfying the constraint.
# Returns "version\nrepo\nref" ("" when nothing matches).
def index_select(dep, cons, offline)
  gf = File.join(index_dir(offline), "packages", dep + ".toml")
  spin_die("not in the index: " + dep + " (spin add " + dep + " --git URL is the escape hatch)") unless File.exist?(gf)
  gdoc = TomlDoc.parse(File.read(gf))
  repo = gdoc.get("", "repo")
  spin_die("index entry for " + dep + " lacks a repo") if repo == ""
  n = gdoc.array_len("release")
  sel_v = ""
  sel_r = ""
  i = 0
  while i < n
    t = "release." + i.to_s
    v = gdoc.get(t, "version")
    r = gdoc.get(t, "ref")
    if v != "" && r != "" && version_satisfies(v, cons)
      if sel_v == "" || vcmp(v, sel_v) < 0
        sel_v = v
        sel_r = r
      end
    end
    i += 1
  end
  spin_die("no release of " + dep + " satisfies " + (cons == "" ? "*" : cons)) if sel_v == ""
  # R8: surface recorded probe results for the selected release. A fail
  # recorded against THIS compiler build is a strong warning; the newest
  # fail against any build still warns. Never an error -- the build tells.
  myrev = spinel_rev
  exact = ""
  latest = ""
  latest_detail = ""
  pi = 0
  while pi < gdoc.array_len("probe")
    t2 = "probe." + pi.to_s
    if gdoc.get(t2, "version") == sel_v
      r2 = gdoc.get(t2, "result")
      latest = r2
      latest_detail = gdoc.get(t2, "detail")
      exact = r2 if myrev != "" && gdoc.get(t2, "spinel") == myrev
    end
    pi += 1
  end
  if exact == "fail"
    $stderr.puts "spin: warning: " + dep + " " + sel_v + " is recorded FAILING with this compiler build" + (latest_detail == "" ? "" : " (" + latest_detail + ")")
  elsif exact == "" && latest == "fail"
    $stderr.puts "spin: warning: " + dep + " " + sel_v + "'s newest probe is a fail" + (latest_detail == "" ? "" : " (" + latest_detail + ")")
  end
  sel_v + "\n" + repo + "\n" + sel_r
end

# --- carried native C (M2) ----------------------------------------------------
# A package may carry .c/.h sources (R6). spin compiles each .c once into the
# shared cache, keyed by (package, version, toolchain), and hands the objects to
# spinel via --link; the compiler itself never touches package C. Objects are
# project-independent (carried C is not specialized by inference).

def native_cc
  c = ENV["CC"].to_s
  c == "" ? "cc" : c
end

# A filename-safe name for the compiler, for the object cache's directory. CC
# is a command line, not a path: "sccache gcc" and "/usr/lib/ccache/cc" both
# have to reduce to something a directory can be called.
def cc_cache_key
  k = ""
  native_cc.each_char do |ch|
    ok = (ch >= "a" && ch <= "z") || (ch >= "A" && ch <= "Z") ||
         (ch >= "0" && ch <= "9") || ch == "-" || ch == "_" || ch == "."
    k += ok ? ch : "_"
  end
  k
end

# The public runtime headers ship beside the compiler: dev tree ../lib,
# installed tree ./lib. $0 does not resolve symlinks, so when spin runs as
# the /usr/local/bin/spin symlink the sibling is the /usr/local/bin/spinel
# symlink -- probe the install layout (<prefix>/lib/spinel/lib) from there.
# Where the runtime headers are: package C includes "spinel/runtime.h", and
# the compiler ships them beside itself. Resolved from the compiler's own
# path, which is why a PATH-resolved compiler has to be located rather than
# given up on -- `spin install` puts spin in ~/.local/bin with no spinel
# beside it, and returning "" there dropped -I <spinel>/lib from every
# package-C compile. The package then failed on a missing runtime.h while the
# same package built fine from a tree where the two sat together (#4115).
#
# SPINEL_HDR_DIR overrides the search outright, for a layout this cannot guess.
def spinel_hdr_dir
  env = ENV["SPINEL_HDR_DIR"].to_s
  return env if env != "" && File.exist?(File.join(env, "spinel_rt.h"))
  bin = spinel_bin
  if bin == "spinel"
    found = which("spinel")
    # PATH returns the symlink as-is; the lib probe below then walks
    # relative to a symlinked path and misses the install tree.
    found = File.realpath(found) if found != "" && File.symlink?(found)
    bin = found
  end
  return "" if bin == ""
  d = File.expand_path("..", bin)
  a = File.join(d, "lib")
  return a if File.exist?(File.join(a, "spinel_rt.h"))
  up = File.expand_path("..", d)
  b = File.join(up, "lib")
  return b if File.exist?(File.join(b, "spinel_rt.h"))
  c = File.join(up, "lib/spinel/lib")
  return c if File.exist?(File.join(c, "spinel_rt.h"))
  ""
end

# Newline-packed absolute paths kept out of the per-file cc sweep and out of
# the staleness scan that decides whether the cached objects are current.
#
# Two sources. A [[build]] workdir is compiled by the package's own build
# system, so it is not carried C: the sweep must not touch it (no include
# path, and it would hard-fail before [[build]] ever runs -- the toy tinynn
# bounce, #1845). And `[package] exclude` is the author saying a path is not
# part of this build at all. Nothing else can say it: `.rb` enters the build by
# require-reachability, `.c` enters by presence, so an application whose
# repository also holds a C program of its own -- a `main()` beside the Ruby --
# had no way to keep it out (#4105).
#
# The globs are expanded here rather than matched at each candidate, so
# path_excluded? stays an exact compare, and naming a directory prunes its
# whole subtree (collect_c consults it before it recurses).
def native_excludes(dir)
  mf = File.join(dir, "spin.toml")
  return "" unless File.exist?(mf)
  toml = TomlDoc.parse(File.read(mf))
  out = ""
  i = 0
  while i < toml.array_len("build")
    wd = toml.get("build." + i.to_s, "workdir")
    if wd != ""
      out += "\n" unless out == ""
      out += File.expand_path(wd, dir)
    end
    i += 1
  end
  toml.get_array("package", "exclude").split("\n").each do |g|
    next if g == ""
    Dir.glob(File.join(dir, g)).each do |hit|
      out += "\n" unless out == ""
      out += hit
    end
  end
  out
end

def path_excluded?(p2, excl)
  excl.split("\n").each { |x| return true if x != "" && p2 == x }
  false
end

# Same exact compare against any newline-packed path list.
def path_in_list?(p2, list)
  list.split("\n").each { |x| return true if x != "" && p2 == x }
  false
end

# A .c that spinel itself emitted is never carried C. It defines main(), and
# through the internal spinel_rt.h it defines a copy of the runtime's
# non-static surface too, so compiling it collides with the generated TU on
# both -- dozens of multiple-definition lines naming symbols, never the file
# that brought them, which is the "very difficult to figure out why" of #4362.
# One `spinel foo.rb -c -o out.c` inside the tree is enough: the compiler's
# own output is otherwise the build's input.
#
# Skipping it is what the author meant, but it is said out loud rather than
# done quietly, because the file may be sitting ON TOP of the source it
# overwrote -- and there the quiet answer would be an undefined symbol instead.
SPINEL_C_BANNER = "/* Generated by Spinel AOT compiler */"
def spinel_emitted_c?(path)
  File.read(path, SPINEL_C_BANNER.length).to_s == SPINEL_C_BANNER
end

# newline-packed .c paths found by walking the tree (an [] accumulator arg would
# go poly-array and box the paths -- same tuple trap as dep_srcs). This is no
# longer what enters the build: it is the census `native_sources` reports
# against, so a `.c` nobody declared is named rather than silently ignored.
def collect_c(dir, excl)
  out = ""
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    next if path_excluded?(p2, excl)
    if File.directory?(p2)
      out += collect_c(p2, excl)
    elsif e.end_with?(".c")
      out += p2 + "\n"
    end
  end
  out
end

# What a package compiles is what its manifest names:
#
#     [package]
#     sources = ["sp_json.c"]      # or ["*.c"], or ["native/*.c"]
#
# `.c` used to enter the build by PRESENCE while `.rb` entered by
# require-reachability, and that asymmetry is what #4362 was about: a file the
# author never meant as a source -- spinel's own output, or a scratch program an
# agent left beside the sources -- was compiled anyway, and the diagnosis was a
# multiple-definition wall that named symbols rather than the file. Now both
# halves of a package are declared: `.rb` by being required, `.c` by being
# listed. No `sources` key means the package carries no C.
#
# Globs are allowed and expanded here, so `path_excluded?` and the undeclared
# census below stay exact compares. `*.c` is deliberately expressible: it is the
# old behaviour, minus spinel's own output, which the banner check still removes
# whether or not a glob matched it -- an allow-list cannot recognise a generated
# file that landed under a name the author declared.
def native_sources(dir, excl)
  mf = File.join(dir, "spin.toml")
  return "" unless File.exist?(mf)
  toml = TomlDoc.parse(File.read(mf))
  out = ""
  toml.get_array("package", "sources").split("\n").each do |g|
    next if g == ""
    hits = 0
    Dir.glob(File.join(dir, g)).each do |p2|
      next if File.directory?(p2)
      next unless p2.end_with?(".c")
      next if path_excluded?(p2, excl)
      hits += 1
      if spinel_emitted_c?(p2)
        $spin_emitted_c += p2 + "\n"
      else
        out += p2 + "\n"
      end
    end
    $spin_empty_source_glob += g + "\n" if hits == 0
  end
  # Everything else in the tree is named rather than dropped in silence. The
  # banner check runs here too: an emitted file needs its own message whether or
  # not a glob happened to reach it, because it may be sitting on top of the
  # source it overwrote and "not declared" would be the wrong thing to say.
  collect_c(dir, excl).split("\n").each do |c|
    next if c == ""
    next if path_in_list?(c, out) || path_in_list?(c, $spin_emitted_c)
    if spinel_emitted_c?(c)
      $spin_emitted_c += c + "\n"
    else
      $spin_undeclared_c += c + "\n"
    end
  end
  out
end

# Headers are reachable by inclusion rather than declared, so the staleness
# scan still walks the tree for `.h`. The `.c` side is the declared list, folded
# in by the caller: a scratch file nobody compiles must not force a rebuild.
def newest_native_input(dir, newest, excl)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    next if path_excluded?(p2, excl)
    if File.directory?(p2)
      newest = newest_native_input(p2, newest, excl)
    elsif e.end_with?(".h")
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

# Where a package's compiled objects live. Shared across projects and keyed by
# (package, version, toolchain), so the same package is not rebuilt for every
# consumer.
#
# SPIN_NATIVE_CACHE relocates it. Two reasons to want that, both from #4115:
# a build that must not write outside its own tree, and a debugging session
# where the answer differs depending on whether an object is already there --
# pointing it at a scratch directory makes every run start from the same
# state. SPIN_NO_NATIVE_CACHE=1 goes further and rebuilds every time.
def native_cache_dir(key)
  override = ENV["SPIN_NATIVE_CACHE"].to_s
  if override != ""
    d = File.expand_path(override)
    mkdir_p_path(d)
    d = File.join(d, key)
    Dir.mkdir(d) unless Dir.exist?(d)
    return d
  end
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "native")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, key)
  Dir.mkdir(d) unless Dir.exist?(d)
  d
end

# Compile one package's carried C into the cache; returns the object list
# of PLAIN (.o) entries only. For each .c, a companion "<stem>_mt.o" is
# also built with the same -DSP_THREADS -ftls-model=initial-exec the
# spinel compiler uses for the main binary when the program uses threads
# (lib/sp_process.c and lib/sp_alloc.c read runtime globals through these;
# without the matching thread-local storage class the link fails). Both
# variants land in the same cache directory; spinel's linker (src/main.c)
# computes <stem>_mt.o from each --link and prefers it when the program
# uses threads, falling back to the plain one otherwise. So one build
# serves either kind of project, and the --link list is unchanged.
def native_objs_for(name, dir, version)
  excl = native_excludes(dir)
  $spin_emitted_c = ""
  $spin_undeclared_c = ""
  $spin_empty_source_glob = ""
  cs = native_sources(dir, excl)
  $spin_emitted_c.split("\n").each do |g|
    next if g == ""
    rel = g[dir.length + 1..-1].to_s
    $stderr.puts "spin: " + name + "/" + rel + " is spinel's own output and was NOT compiled."
    $stderr.puts "spin:   delete it, or move it out of the package tree. If it overwrote a"
    $stderr.puts "spin:   source of the same name, restore that source: it is gone."
  end
  # A forgotten declaration would otherwise surface as an undefined symbol at
  # link, which is the same "very difficult to figure out why" this change is
  # meant to remove, with the sign flipped. Say it here, where the file is
  # still in hand.
  $spin_undeclared_c.split("\n").each do |g|
    next if g == ""
    rel = g[dir.length + 1..-1].to_s
    $stderr.puts "spin: " + name + "/" + rel + " is not named by [package] sources and was NOT compiled."
  end
  $spin_empty_source_glob.split("\n").each do |g|
    next if g == ""
    $stderr.puts "spin: " + name + ": [package] sources entry \"" + g + "\" matched no .c file."
  end
  return [] if cs == ""
  # The cache key names a DIRECTORY, so the compiler part of it has to be
  # spellable as one. A CC of "sccache gcc" or "ccache cc" -- what CI and most
  # developer setups use -- put a space in the path, and the unquoted -o
  # argument below then split into two words at it.
  odir = native_cache_dir(name + "-" + version + "-" + cc_cache_key)
  hdr = spinel_hdr_dir
  hnew = newest_native_input(dir, 0, excl)
  cs.split("\n").each do |c|
    next if c == ""
    m = File.mtime(c).to_i
    hnew = m if m > hnew
  end
  # The runtime headers are part of the object's inputs: a package's C is
  # compiled against spinel_rt.h and the structures the allocators inline
  # (sp_alloc.h's per-worker string slot, sp_gc.h's object heap), and an
  # object built before a layout change writes the old layout into the new
  # runtime -- silently, until a string comes back wrong. The Makefile lists
  # the same four for the bundled packages; the cache keys on them too.
  if hdr != ""
    ["spinel/runtime.h", "spinel_rt.h", "sp_alloc.h", "sp_gc.h", "sp_types.h"].each do |h|
      f = File.join(hdr, h)
      next unless File.exist?(f)
      m = File.mtime(f).to_i
      hnew = m if m > hnew
    end
  end
  objs = []
  cs.split("\n").each do |c|
    rel = c[dir.length + 1..-1].to_s
    # The cache MIRRORS the package tree. Flattening "/" to "_" was not
    # injective: `a/util.c` and `a_util.c` both named `a_util.o`, the second
    # compile overwrote the first, and the link asked for a symbol whose
    # object had been replaced -- with nothing said at any point. A directory
    # cannot collide with a file of the same name, so mirroring settles it,
    # and the path stays one a person can look for.
    base = rel[0..-3]
    [["", ""], ["_mt", " -DSP_THREADS -ftls-model=initial-exec"]].each do |suffix, flags|
      o = File.join(odir, base + suffix + ".o")
      mkdir_p_path(File.dirname(o)) if rel.include?("/")
      if !File.exist?(o) || File.mtime(o).to_i < hnew || ENV["SPIN_NO_NATIVE_CACHE"].to_s != ""
        cmd = native_cc + " -O2" + flags + " -c '#{c}' -I '#{dir}'"
        cmd += " -I '#{hdr}'" if hdr != ""
        cmd += " -o '#{o}'"
        spin_die("native compile failed: " + rel + " (" + name + ")") unless run_command(cmd)
        # stderr, not stdout: `spin flags` prints a flag string on stdout and
        # a cold cache compiles here first, so progress on stdout would be
        # spliced into the flags the caller passes to the compiler (#4105).
        # In --verbose the run_command echo already shows the full command,
        # so the short status line is redundant there.
        # the suffix names the VARIANT, so it goes after the file name rather
        # than onto it: "nat/nat.c_mt" is not a path anyone can look for
        line = "cc #{name}/#{rel}"
        line += " (threaded)" if suffix != ""
        $stderr.puts line unless $spin_verbose
      end
      # Only the plain .o goes on the --link list; spinel computes the
      # _mt variant from it. Including _mt.o here would have the linker
      # add it twice (once via dedup of the _mt->_mt path, once via
      # the plain->_mt rewrite) and break with multiple-definition errors.
      objs.push(o) if suffix == ""
    end
  end
  objs
end

# --- declared native build steps ([[build]]) ----------------------------------
#
# A package may vendor an external project with its own build system (cmake,
# make) that carried-C's per-file CC cannot express. `[[build]]` entries in
# its spin.toml declare the step: a command run in a scratch copy of
# `workdir`, optional `patches` applied to that copy first, and `artifacts`
# the run must produce. Entries run at DEPENDENT-APPLICATION BUILD TIME only
# (never at fetch, preserving R2), are consented explicitly, and cache into
# the shared content-keyed native cache. Artifacts reach the link line via
# `[native] libs` entries, where `${build.out}` expands to the package's
# artifact directory -- linking stays on the existing shape-2 surface.

def spin_config_dir
  base = ENV["XDG_CONFIG_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".config") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d
end

def native_trust_file
  File.join(spin_config_dir, "trust")
end

def native_trusted?(name)
  f = native_trust_file
  return false unless File.exist?(f)
  File.read(f).split("\n").include?(name)
end

def native_trust!(name)
  f = native_trust_file
  have = File.exist?(f) ? File.read(f) : ""
  return if have.split("\n").include?(name)
  File.write(f, have + name + "\n")
end

# A package running an external build system is a trust decision the consumer
# makes: the flag/env for this run, a recorded `spin trust <name>`, or -- on
# an interactive terminal -- a per-run prompt. There is deliberately no
# silent default (cargo's build.rs runs unprompted; we don't copy that), and
# a non-interactive build (CI, pipes) never waits on a prompt.
def ensure_native_allowed(name, command)
  return if ENV["SPIN_ALLOW_NATIVE_BUILD"].to_s != ""
  return if native_trusted?(name)
  if $stdin.tty?
    $stderr.puts "spin: package '#{name}' declares a native build step:"
    $stderr.puts "  #{command}"
    $stderr.print "Allow? [y/N/always] "
    ans = $stdin.gets
    ans = ans.nil? ? "" : ans.strip.downcase
    if ans == "always"
      native_trust!(name)
      return
    end
    return if ans == "y" || ans == "yes"
    spin_die("native build not allowed (#{name})")
  end
  $stderr.puts "spin: package '#{name}' declares a native build step:"
  $stderr.puts "  #{command}"
  $stderr.puts "Allow it with --allow-native-build (this run),"
  $stderr.puts "SPIN_ALLOW_NATIVE_BUILD=1 (CI), or `spin trust #{name}` (always)."
  exit 1
end

# Content hasher: prefer sha256, fall back for platforms without coreutils
# naming (macOS ships shasum). cksum is the POSIX floor -- weak, but this is
# a cache key, not a security boundary.
def native_hasher
  return $spin_hasher if $spin_hasher != ""
  ["sha256sum", "shasum -a 256", "cksum"].each do |h|
    probe = sh_read("command -v " + h.split(" ")[0])
    if probe != ""
      $spin_hasher = h
      return h
    end
  end
  $spin_hasher = "cksum"
  "cksum"
end

def native_hash_pipe(shell_producer)
  out = sh_read("(" + shell_producer + ") | " + native_hasher)
  out.split(" ")[0].to_s
end

# Content hash of a directory tree: the sorted file list plus every file's
# bytes. Rename-only changes and content changes both move the key.
def native_tree_hash(dir, excludes = "")
  # dot-entries (.git and friends) are excluded, consistent with collect_c /
  # newest_mtime: a workdir that is a real git clone must not churn the key
  # on fetch metadata, and stale VCS state is not a build input. An entry's
  # own `exclude` globs (build output dirs riding inside a dev workdir) are
  # pruned the same way. -mindepth 1: find's first entry is `.` itself, which
  # matches -name '.*' and would prune the WHOLE tree (every workdir hashed
  # as empty input, so source edits never moved the key).
  pr = "-name '.*'"
  excludes.split("\n").each do |g|
    next if g == ""
    spin_die("[[build]] exclude may not contain quotes: #{g}") if g.include?("'")
    pr += " -o -path './#{g}'"
  end
  lst = "cd #{dir} && find . -mindepth 1 \\( #{pr} \\) -prune -o -type f -print | LC_ALL=C sort"
  native_hash_pipe(lst + " ; (" + lst + ") | while read f; do cat \"$f\"; done")
end

def native_out_dir(name, version, key)
  native_cache_dir(name + "-" + version + "-build-" + key)
end

# Run one package's [[build]] entries (consented, content-cached) and return
# its `[native] libs` for the link line, newline-packed, with ${build.out}
# expanded to the artifact directory. "" when the package declares no build.
# Entries gated on `features` run only when every named feature is in the
# package's own `[features] default` set (consumer-side enablement is a
# later slice); a lib entry whose artifact was feature-skipped is dropped.
def native_build_libs_for(name, dir, version, consumer_feats)
  mf = File.join(dir, "spin.toml")
  return "" unless File.exist?(mf)
  toml = TomlDoc.parse(File.read(mf))
  n = toml.array_len("build")
  return "" if n == 0
  # enabled = the package's own defaults plus what the consuming application
  # switched on for this dependency (cargo-style: features live in the
  # manifest, the source of truth; the lock stays resolution-only)
  enabled = toml.get_array("features", "default")
  consumer_feats.split("\n").each do |cf|
    next if cf == "" || enabled.split("\n").include?(cf)
    enabled += "\n" unless enabled == ""
    enabled += cf
  end

  # one artifact dir per package, keyed over every entry's inputs
  keysrc = "cc=" + File.basename(native_cc) + "\nfeatures=" + enabled
  i = 0
  while i < n
    t = "build." + i.to_s
    wd = toml.get(t, "workdir")
    spin_die("[[build]] entry #{i} of #{name}: workdir is required") if wd == ""
    wdir = File.join(dir, wd)
    spin_die("[[build]] entry #{i} of #{name}: no such workdir #{wd}") unless File.directory?(wdir)
    keysrc += "\nworkdir=" + wd + "@" + native_tree_hash(wdir, toml.get_array(t, "exclude"))
    keysrc += "\nexclude=" + toml.get_array(t, "exclude")
    keysrc += "\ncommand=" + toml.get(t, "command")
    keysrc += "\nartifacts=" + toml.get_array(t, "artifacts")
    keysrc += "\nfeatures=" + toml.get_array(t, "features")
    toml.get_array(t, "patches").split("\n").each do |pg|
      Dir.glob(File.join(dir, pg)).sort.each do |pf|
        keysrc += "\npatch=" + File.basename(pf) + "@" + native_hash_pipe("cat #{pf}")
      end
    end
    i += 1
  end
  keyf = "/tmp/spin_key_#{Process.pid}"
  File.write(keyf, keysrc)
  key = native_hash_pipe("cat " + keyf)[0..15]
  File.delete(keyf)
  out = native_out_dir(name, version, key)

  i = 0
  while i < n
    t = "build." + i.to_s
    gates = toml.get_array(t, "features")
    skip = false
    gates.split("\n").each { |g| skip = true unless enabled.split("\n").include?(g) }
    if skip
      i += 1
      next
    end
    arts = toml.get_array(t, "artifacts")
    spin_die("[[build]] entry #{i} of #{name}: artifacts is required") if arts == ""
    # Artifacts keep their entry-relative path under ${build.out} (a bare
    # `libggml.a` still lands at the top). Authors namespace colliding names
    # (a CPU and a CUDA libggml.a) by building/declaring them under distinct
    # subpaths -- no per-entry output dir needed (#1845 bounce 3).
    missing = false
    arts.split("\n").each { |a| missing = true unless File.exist?(File.join(out, a)) }
    unless missing
      i += 1
      next   # cached: every artifact already present for this key
    end
    cmdline = toml.get(t, "command")
    spin_die("[[build]] entry #{i} of #{name}: command is required") if cmdline == ""
    # ${build.out} expands in the command too, so a later entry can compile
    # against an earlier entry's artifacts (headers, archives) without
    # snapshotting them into its own tree (#1845 bounce 2). Entries run in
    # declaration order.
    cmdline = cmdline.gsub("${build.out}", out)
    ensure_native_allowed(name, cmdline)
    # scratch copy: the vendored tree stays a read-only input. Dot-entries
    # (.git and friends) are dropped from the copy, consistent with the tree
    # hash -- VCS state is not a build input and a real clone's .git is heavy.
    scratch = out + ".scratch"
    system("rm -rf #{scratch}")
    # -H dereferences the WORKDIR OPERAND itself: a symlinked workdir must
    # copy the pointed-to tree, or the "scratch" is a symlink and every
    # later step (patch, the build command) writes through into the live
    # checkout the link points at. Symlinks inside the tree stay links.
    spin_die("native build: cannot copy #{name}'s workdir") unless system("cp -RH #{File.join(dir, toml.get(t, 'workdir'))} #{scratch}")
    spin_die("native build: #{name}'s workdir copied as a symlink, not a tree") unless File.directory?(scratch) && !File.symlink?(scratch)
    system("find #{scratch} -mindepth 1 -name '.*' -prune -exec rm -rf {} + 2>/dev/null")
    toml.get_array(t, "exclude").split("\n").each do |g|
      next if g == ""
      system("cd #{scratch} && rm -rf #{g}") if File.directory?(scratch)
    end
    toml.get_array(t, "patches").split("\n").each do |pg|
      Dir.glob(File.join(dir, pg)).sort.each do |pf|
        unless system("patch -s -p1 -d #{scratch} < #{pf}")
          system("rm -rf #{scratch}")
          spin_die("native build: patch failed: #{File.basename(pf)} (#{name})")
        end
      end
    end
    puts "native #{name}: #{cmdline}"
    unless system("cd #{scratch} && ( #{cmdline} )")
      system("rm -rf #{scratch}")
      spin_die("native build failed (#{name})")
    end
    arts.split("\n").each do |a|
      built = File.join(scratch, a)
      unless File.exist?(built)
        system("rm -rf #{scratch}")
        spin_die("native build of #{name} did not produce declared artifact: #{a}")
      end
      dest = File.join(out, a)
      ddir = File.dirname(dest)
      system("mkdir -p #{ddir}") unless Dir.exist?(ddir)
      system("cp #{built} #{dest}")
    end
    system("rm -rf #{scratch}")
    i += 1
  end

  # Artifact names declared by feature-DISABLED entries: a [native] libs
  # reference to one of these silently drops out (the feature is off); a
  # missing path declared by NO entry at all is an authoring error and dies
  # loud (a stale path otherwise surfaces as hundreds of undefined symbols
  # at link time, #2010).
  gated_off = ""
  i = 0
  while i < n
    t = "build." + i.to_s
    gates = toml.get_array(t, "features")
    skip = false
    gates.split("\n").each { |g| skip = true unless enabled.split("\n").include?(g) }
    if skip
      toml.get_array(t, "artifacts").split("\n").each do |a|
        gated_off += "\n" unless gated_off == ""
        gated_off += a
      end
    end
    i += 1
  end
  libs = ""
  toml.get_array("native", "libs").split("\n").each do |l|
    path = l.gsub("${build.out}", out)
    unless File.exist?(path)
      rel = l.gsub("${build.out}/", "").gsub("${build.out}", "")
      next if gated_off.split("\n").include?(rel)   # feature-skipped: drops out
      spin_die("[native] libs of #{name}: #{l} was not produced by any [[build]] entry")
    end
    libs += "\n" unless libs == ""
    libs += path
  end
  libs
end

# --- shared cache & git sources (M1) -----------------------------------------

def cache_packages_dir
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  g = File.join(d, "packages")
  Dir.mkdir(g) unless Dir.exist?(g)
  g
end

def sh_read(cmd)
  tmp = "/tmp/spin_out_#{Process.pid}"
  system(cmd + " > " + tmp + " 2>/dev/null")
  out = File.exist?(tmp) ? File.read(tmp).strip : ""
  File.delete(tmp) if File.exist?(tmp)
  out
end

def gem_version_of(dir)
  mf = File.join(dir, "spin.toml")
  return "0.0.0" unless File.exist?(mf)
  v = TomlDoc.parse(File.read(mf)).get("package", "version")
  v == "" ? "0.0.0" : v
end

# Fetch (or reuse) a git source; returns "dir\nversion\nsha".
def git_fetch(name, url, ref, want_sha)
  pkgs = cache_packages_dir
  # a previously locked SHA that is already cached wins (offline path)
  if want_sha != ""
    hits = Dir.glob(pkgs + "/" + name + "-*")
    hits.each do |h|
      stamp = File.join(h, ".spin-sha")
      next unless File.exist?(stamp)
      if File.read(stamp).strip == want_sha
        return h + "\n" + gem_version_of(h) + "\n" + want_sha
      end
    end
  end
  # Staging is PER PROCESS. A path fixed per package name is shared by every
  # concurrent caller in one tree: each `rm -rf`s what another is cloning into,
  # and the second clone finds a non-empty directory and dies. Eight parallel
  # `spin flags` left seven failures and seven empty flag strings, which
  # `$(shell ...)` in a Makefile turns into a silent build with no -I at all
  # (#4393).
  tmp = File.join(pkgs, ".fetch-" + name + "-" + Process.pid.to_s)
  system("rm -rf " + tmp)
  cloned = false
  if want_sha != ""
    # materialize the exact pinned/selected commit: fetch it directly
    # (works on file:// and GitHub); a server refusing SHA fetches falls
    # back to the full clone + checkout below.
    cloned = system("mkdir -p " + tmp) &&
             system("git -C " + tmp + " init -q 2>/dev/null") &&
             system("git -C " + tmp + " fetch -q --depth 1 " + url + " " + want_sha + " 2>/dev/null") &&
             system("git -C " + tmp + " checkout -q FETCH_HEAD 2>/dev/null")
    system("rm -rf " + tmp) unless cloned
  end
  unless cloned
    refarg = ref == "" ? "" : " --branch " + ref
    depth = want_sha == "" ? " --depth 1" : ""
    ok = system("git clone -q" + depth + refarg + " " + url + " " + tmp)
    spin_die("fetch failed: git clone " + url) unless ok
    if want_sha != ""
      okc = system("git -C " + tmp + " checkout -q " + want_sha)
      spin_die("fetch failed: " + name + " has no commit " + want_sha) unless okc
    end
  end
  sha = sh_read("git -C " + tmp + " rev-parse HEAD")
  spin_die("fetch failed: no HEAD sha for " + url) if sha == ""
  spin_die("fetch verify failed: wanted " + want_sha + ", got " + sha) if want_sha != "" && sha != want_sha
  ver = gem_version_of(tmp)
  final = File.join(pkgs, name + "-" + ver)
  stamp = File.join(final, ".spin-sha")
  # Someone else may have published this exact version while we fetched. Take
  # theirs rather than removing it: a concurrent caller's `-I` already points
  # inside that directory, and `rm -rf` on it pulled the include path out from
  # under a build that was using it.
  if File.directory?(final) && File.exist?(stamp) && File.read(stamp).strip == sha
    system("rm -rf " + tmp)
    return final + "\n" + ver + "\n" + sha
  end
  system("rm -rf " + File.join(tmp, ".git"))
  File.write(File.join(tmp, ".spin-sha"), sha + "\n")
  # rename(2) is atomic and REFUSES to nest into an existing directory, which
  # is what makes losing the race detectable rather than silent -- `mv` would
  # have moved our tree INSIDE theirs.
  ok2 = false
  begin
    File.rename(tmp, final)
    ok2 = true
  rescue StandardError
    if File.exist?(stamp) && File.read(stamp).strip == sha
      system("rm -rf " + tmp)          # they published what we wanted
      ok2 = true
    else
      system("rm -rf " + final)        # a stale tree at another sha
      ok2 = system("mv " + tmp + " " + final)
    end
  end
  spin_die("fetch failed: cannot place " + final) unless ok2
  final + "\n" + ver + "\n" + sha
end

# --- spin.lock ----------------------------------------------------------------

def write_lock(root, lines)
  body = "# generated by spin lock -- diff me, don't edit me\n"
  lines.each { |l| body += l }
  File.write(File.join(root, "spin.lock"), body)
end

# Resolve all [dependencies] of the manifest at `root` (recursively through
# fetched gems), preferring SHAs recorded in spin.lock. Returns newline-packed
# records "name\tdir\tversion\tgit\tsha_or_path" joined by \n.
def resolve_deps(root, offline)
  root0 = root
  lf = File.join(root, "spin.lock")
  lock = TomlDoc.parse("")
  lock = TomlDoc.parse(File.read(lf)) if File.exist?(lf)
  out = ""
  seen = { "" => "" }
  queue = [root]
  qdirs = { root => "" }
  while queue.length > 0
    cur = queue.shift.to_s
    mf = File.join(cur, "spin.toml")
    next unless File.exist?(mf)
    toml = TomlDoc.parse(File.read(mf))
    toml.table_keys("dependencies").each do |dep|
      next if seen.key?(dep)
      seen[dep] = "1"
      pth = toml.get_inline("dependencies", dep, "path")
      url = toml.get_inline("dependencies", dep, "git")
      ref = toml.get_inline("dependencies", dep, "ref")
      if pth != ""
        d2 = File.expand_path(pth, cur)
        spin_die("dependency " + dep + ": path not found: " + pth) unless File.directory?(d2)
        out += dep + "\t" + d2 + "\t" + gem_version_of(d2) + "\tpath\t" + pth + "\n"
        queue.push(d2)
      elsif url != ""
        want = lock.get("lock." + dep, "ref")
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_packages_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if want != "" && File.exist?(st) && File.read(st).strip == want
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/packages") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + want.to_s
        else
          rec = git_fetch(dep, url, ref, want)
        end
        parts = rec.split("\n")
        # want may be "" (no spin.lock yet): split drops the trailing empty
        # field, so read the SHA defensively rather than trusting parts[2].
        sha = parts.length > 2 ? parts[2] : ""
        out += dep + "\t" + parts[0] + "\t" + parts[1] + "\tgit\t" + url + "\x01" + sha + "\n"
        queue.push(parts[0])
      else
        # a plain string value is an index constraint: foo = "~> 1.2" / "*";
        # with per-dep features it moves inline: foo = { version = "*", features = [..] }
        cons = toml.get("dependencies", dep)
        cons = toml.get_inline("dependencies", dep, "version") if cons == ""
        sel = index_select(dep, cons, offline).split("\n")
        sel_v = sel[0]
        repo = sel[1]
        sel_r = sel.length > 2 ? sel[2] : ""
        # verify-not-select: a lock ref that still satisfies the constraint
        # pins the build; one that no longer does is reselected (spin lock
        # rewrites the pin).
        want = lock.get("lock." + dep, "ref")
        lv = lock.get("lock." + dep, "version")
        if want != "" && version_satisfies(lv, cons)
          sel_r = want
          sel_v = lv
        elsif want != ""
          $stderr.puts "spin: spin.lock pins " + dep + " " + lv + " outside " + cons + "; reselecting " + sel_v
        end
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_packages_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if sel_r != "" && File.exist?(st) && File.read(st).strip == sel_r
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/packages") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + sel_r
        else
          rec = git_fetch(dep, repo, "", sel_r)
        end
        parts = rec.split("\n")
        sha = parts.length > 2 ? parts[2] : sel_r
        out += dep + "\t" + parts[0] + "\t" + sel_v + "\tindex\t" + repo + "\x01" + sha + "\x01" + cons + "\n"
        queue.push(parts[0])
      end
    end
  end
  out
end

# --- manifest ----------------------------------------------------------------

class Project
  attr_reader :root, :name, :dep_paths, :allocator

  def initialize(root)
    @root = root
    toml = TomlDoc.parse(File.read(File.join(root, "spin.toml")))
    nm = toml.get("package", "name")
    if nm == ""
      base = File.basename(root)
      base = base[7..-1] if base.start_with?("spinel-")
      nm = base
    end
    @name = nm
    # `[package] allocator`. A malloc replacement is a property of the
    # PROGRAM, not of the compiler: a server allocates for its whole life
    # where a batch job barely allocates at all, and only the manifest knows
    # which this is. "" (the default) and "system" both mean the platform's
    # own allocator. Read from THIS manifest only: a dependent never compiles
    # a dependency's bin/, so a dependency's choice cannot reach the program
    # being built -- and a process has one allocator, which is not a decision
    # a library makes for its dependents.
    @allocator = toml.get("package", "allocator")
    # per-dependency feature enablement from THIS manifest's [dependencies]
    # inline specs (dep = { ..., features = ["cuda"] }); root-level only --
    # transitive feature unification is out of scope
    @dep_features = { "" => "" }
    toml.table_keys("dependencies").each do |dep|
      f = toml.inline_array("dependencies", dep, "features")
      @dep_features[dep] = f if f != ""
    end
    @dep_paths = []
    @dep_records = resolve_deps(root, ENV["SPIN_OFFLINE"].to_s.length > 0)
    # tab-packed name\tdir\tversion records (an array of tuples would go
    # poly and poison the string params downstream)
    @dep_srcs = @name + "\t" + root + "\t" + gem_version_of(root)
    @dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      # prefer a vendored copy when present
      vd = File.join(root, "vendor/packages", f[0] + "-" + f[2])
      d = File.directory?(vd) ? vd : f[1]
      @dep_paths.push(d)
      @dep_srcs += "\n" + f[0] + "\t" + d + "\t" + f[2]
    end
  end

  # The source a carried-C object was compiled from, or "" when there is no
  # telling. A bundled package (the compiler's own packages/) compiles beside
  # its source. A dependency compiles into the shared object cache, one
  # directory per (package, version, compiler) whose layout mirrors the
  # package tree -- so the object's path below that directory names its source
  # within the package. `_mt` marks the threaded variant of one source file.
  def native_source_for(obj)
    base = File.basename(obj)
    stem = base[0, base.length - 2]
    stem = stem[0, stem.length - 3] if stem.length > 3 && stem[stem.length - 3, 3] == "_mt"
    beside = File.join(File.dirname(obj), stem + ".c")
    return beside if File.exist?(beside)
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      key = "/" + f[0] + "-" + f[2] + "-" + cc_cache_key + "/"
      at = obj.index(key)
      next if at.nil?
      rel = obj[(at + key.length)..-1].to_s
      rel = rel[0, rel.length - 2]
      rel = rel[0, rel.length - 3] if rel.length > 3 && rel[rel.length - 3, 3] == "_mt"
      src = File.join(f[1], rel + ".c")
      return src if File.exist?(src)
    end
    ""
  end

  # A dep package's native object as "name\tdir\trel": the package, its
  # source tree, and the source's path in it without ".c". "" for an object
  # that is not a dep's (a bundled package's sits beside its source).
  def native_source_parts(obj)
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      key = "/" + f[0] + "-" + f[2] + "-" + cc_cache_key + "/"
      at = obj.index(key)
      next if at.nil?
      rel = obj[(at + key.length)..-1].to_s
      rel = rel[0, rel.length - 2]
      rel = rel[0, rel.length - 3] if rel.length > 3 && rel[rel.length - 3, 3] == "_mt"
      return f[0] + "\t" + f[1] + "\t" + rel if File.exist?(File.join(f[1], rel + ".c"))
    end
    ""
  end

  # carried native C across the root package and every resolved dep (M2)
  def native_objs
    objs = []
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      native_objs_for(f[0], f[1], f[2]).each { |o| objs.push(o) }
    end
    objs
  end

  # declared [[build]] steps across the root package and every resolved dep:
  # runs (or reuses) each package's native build and returns the expanded
  # `[native] libs` link inputs, newline-packed. Memoized -- the staleness
  # check and compile() both consult it, and a cached build is cheap but a
  # cold one is not.
  def native_build_libs
    return @native_build_libs if @native_build_libs != nil
    libs = ""
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      cf = @dep_features.key?(f[0]) ? @dep_features[f[0]] : ""
      got = native_build_libs_for(f[0], f[1], f[2], cf)
      next if got == ""
      libs += "\n" unless libs == ""
      libs += got
    end
    @native_build_libs = libs
    libs
  end

  def dep_records
    @dep_records
  end

  def bins
    out = []
    bd = File.join(@root, "bin")
    return out unless File.directory?(bd)
    Dir.glob(bd + "/*.rb").each do |path|
      out.push(File.basename(path)[0..-4])
    end
    out
  end

  def tests
    out = []
    td = File.join(@root, "test")
    return out unless File.directory?(td)
    Dir.glob(td + "/*.rb").each do |path|
      out.push(File.basename(path))
    end
    out
  end
end

# --- staleness (newest input mtime vs output mtime) --------------------------

# Return the newest source/configuration mtime, excluding generated, vendored,
# and hidden directories from the recursive project scan.
def newest_mtime(dir, newest)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")   # .git and friends
    p2 = File.join(dir, e)
    next if e == "build" || e == "vendor"
    if File.directory?(p2)
      newest = newest_mtime(p2, newest)
    elsif e.end_with?(".rb") || e.end_with?(".rbs") || e.end_with?(".c") || e.end_with?(".h") || e == "spin.toml"
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

# Resolve the compiler and available runtime archives in driver search order.
# These are shared prerequisites for cached outputs and external build systems.
def toolchain_deps
  sb = spinel_bin
  found = which(sb)
  sb = File.realpath(found) if found != ""
  deps = [sb]
  # Match the compiler driver's installed and checkout layouts, in that
  # order: <compiler-dir>/lib, then <compiler-dir>/../lib.
  dir = File.expand_path("..", sb)
  rt = File.join(dir, "lib")
  rt = File.expand_path("../lib", dir) unless File.file?(File.join(rt, "libspinel_rt.a"))
  ["libspinel_rt.a", "libspinel_rt_mt.a"].each do |name|
    path = File.join(rt, name)
    deps << path if File.file?(path)
  end
  deps
end

# Return the newest project, dependency, compiler, or runtime archive mtime
# so build and test caches invalidate after runtime-only toolchain updates.
def inputs_mtime(prj)
  newest = newest_mtime(prj.root, 0)
  prj.dep_paths.each { |d| newest = newest_mtime(d, newest) }
  # Runtime-only changes relink the archives without changing the compiler.
  # Use the same prerequisites we hand external builds through flags --deps.
  toolchain_deps.each do |path|
    newest = File.mtime(path).to_i if File.file?(path) && File.mtime(path).to_i > newest
  end
  newest
end

# --- build -------------------------------------------------------------------

# The spinel command line that compiles `entry` to `out`. Split out from
# compile() so `spin test` can collect commands and run them in parallel.
# Every compiler flag this project implies, with no entry file and no -o: what
# `spin build` passes, and what `spin flags` prints. One producer for both, so
# a Makefile driving the compiler itself gets exactly the build spin would have
# made rather than an approximation of it (#4105).
#
# Every path here is absolute -- find_root walks up from Dir.pwd, path
# dependencies go through File.expand_path, and cache objects are named from
# the cache root -- which matters for `spin flags`, whose caller's working
# directory is its own tree, not this project.
def spin_flags(prj)
  # Inside a spin project the dependency universe is fully known (manifest +
  # lock), so an unresolvable require is a bug, not a maybe: flip the
  # compiler's require gate from warning to hard error. This also makes
  # stdlib features require-gated, i.e. CRuby-style `require "stringio"`
  # before use.
  f = "--require-gate"
  prj.dep_paths.each { |d| f += " -I #{d}" }
  f += " -I #{prj.root}"
  # Feed .rbs sidecars to the compiler's --rbs seed machinery when the project
  # carries any (issue #1788). `.rbs` participates by extension, so a package's
  # type sidecars pin its public surfaces (e.g. a Router#match that would
  # otherwise infer poly) under `spin build`/`test`. --rbs takes one dir and its
  # extractor scans recursively, so the project root covers every sidecar.
  if Dir.glob(File.join(prj.root, "**", "*.rbs")).any?
    f += " --rbs #{prj.root}"
  end
  # Reading these compiles any carried C and runs any declared native build
  # that is not already cached, which is what makes the --link paths real.
  prj.native_objs.each { |o| f += " --link #{o}" }
  prj.native_build_libs.split("\n").each { |l| f += " --link #{l}" if l != "" }
  # The allocator the manifest asked for, as an ordinary link input. Declared
  # and missing is a hard link error, deliberately: the manifest states what
  # the program needs, and an unmet statement should fail the way an
  # unresolvable dependency does rather than quietly building something
  # slower than what was asked for. Ordering is free -- this is a whole -l,
  # not an object the earlier ones resolve symbols against.
  f += " --link -l" + prj.allocator if prj.allocator != "" && prj.allocator != "system"
  f
end

def compile_cmd(prj, entry, out, extra)
  cmd = "#{spinel_bin} #{entry} #{spin_flags(prj)}"
  cmd += " #{extra}" if extra != ""
  # `spin build --debug` / `-g` (or SPIN_DEBUG=1) forwards the compiler's
  # debug build (#line + -g -O0) so the emitted binary is steppable in
  # lldb/gdb; without it there is no way to get a debuggable build through
  # spin (it always compiles release).
  cmd += " --debug" if ENV["SPIN_DEBUG"].to_s != ""
  cmd += " -o #{out}"
  cmd
end

# The hint belongs to ONE failure -- a require nothing provides, which
# `spin add` fixes -- and it used to be printed for every one. On a parse
# error or a type error it was the last line the reader saw and it pointed
# away from the fix (#4136). So the compiler's stderr is captured, replayed,
# and read: the hint follows only when the failure it describes is the one
# that happened.
#
# Capturing costs the live interleaving of compiler warnings with the rest of
# the build output; they arrive together at the end of this target instead.
# That is the price of knowing what the failure was, and warnings are read
# after a build rather than during one.
def compile(prj, entry, out, extra)
  tmp = ENV["TMPDIR"].to_s
  tmp = "/tmp" if tmp == ""
  err = File.join(tmp, "spin-compile-#{Process.pid}.err")
  # The compiler's stderr ends up in `text` either way -- in non-verbose
  # mode by redirecting to a file and reading it after; in verbose mode by
  # spawning with a pipe, streaming each chunk to the operator live, and
  # accumulating it. `text` is then used below for the "cannot load such
  # file" hint. The verbose branch keeps the existing live-streaming
  # behaviour (#4136 in spirit) and the hint check still works.
  text = String.new   # appended below; a "" literal is frozen under spinel
  if $spin_verbose
    # run_command echoes the cmd, streams stderr live, and (with text:)
    # captures the same stderr for the "cannot load such file" hint
    # check below.
    ok = run_command(compile_cmd(prj, entry, out, extra), text: text)
  else
    ok = system(compile_cmd(prj, entry, out, extra) + " 2>#{err}")
    text = File.exist?(err) ? File.read(err) : ""
    $stderr.print text unless text.empty?
    File.unlink(err) if File.exist?(err)
  end
  unless ok
    if text.include?("cannot load such file")
      $stderr.puts "spin: build failed (hint: an unresolved require may need a dependency: spin add <name> --path <dir>)"
    else
      $stderr.puts "spin: build failed"
    end
    exit 1
  end
end

# The compiler modes that produce no executable. `spin build` writes one to
# build/bin/<target> and compile_cmd always appends that `-o`, so these
# contradict it -- and `-c` does so silently: spinel honours both flags and
# writes C SOURCE over the executable's path, which keeps its executable bit
# while the build reports success (#4098).
EMIT_ONLY_FLAGS = ["-c", "-S", "--emit-rbs", "--emit-types",
                   "--emit-symbol-map", "--dump-ast"]

def pack_copy(src, dst)
  File.write(dst, File.read(src))
end

def pack_mkdir(d)
  Dir.mkdir(d) unless Dir.exist?(d)
end

# `spin pack`: a directory that builds the program from C alone -- no spinel,
# no spin, a C compiler and make.
#
# The recipe is DERIVED rather than written. `spinel --print-build` reports the
# ingredients the program REQUIRES -- its defines, include paths, libraries and
# link inputs -- and the Makefile below is those with their paths rewritten to
# the pack's own. A second copy of the build knowledge would drift, and a
# drifted copy breaks only on the machine the pack was sent to, which is the
# least diagnosable place for it to break.
#
# What the compiler does NOT report, and what this therefore does not carry, is
# which cc to run or how this particular build was run. Those are the
# recipient's: someone cross-compiling for another target brings their own
# toolchain and their own optimisation, and would not thank us for a Makefile
# carrying our -Werror, our diagnostic formatting and our linker's spelling of
# section GC. CC and CFLAGS below are defaults, not decisions.
#
# The runtime ships as SOURCE, not as the .a. That is not thoroughness: the
# generated C includes the runtime headers, and 270 of the runtime's functions
# are `static inline` in them, so the recipient compiles 16,765 lines of
# runtime either way. Shipping the .a would save the .c files and cost the
# thing the pack is for -- an archive is built for one platform and one set of
# defines (SP_THREADS above all), and a mismatched one links and then misbehaves
# at run time, because the generated TU writes its own externs and nothing
# cross-checks them.
# The probe's `#include` is spelled with printf's octal escape, so that no `#`
# reaches the Makefile at all. There is no spelling of a literal one that every
# make reads alike: before 4.3 a `#` inside a function invocation starts a
# comment and has to be written `\#`, and 4.3 both stopped treating it as a
# comment and stopped unescaping it, so `\#` resolves to a backslash and a hash
# there. The pack builds with whatever make the RECIPIENT has, so it uses
# neither: `\043` is printf's, not make's, and every version passes it through
# untouched to a printf that writes the same byte.
# The make-time check a THREADED pack carries. The program uses Thread, so its
# runtime is compiled with -DSP_THREADS and needs pthread from the compiler
# that builds it -- the recipient's $(CC), which may be a cross compiler for a
# target that has none. The packer's host says nothing about that, so the
# question is asked where the answer lives: a compile-and-link probe with
# $(CC), and a make error naming the reason, in place of the pthread.h error
# forty files into the build. A pack of a program without threads carries no
# probe and needs no pthread.
def pack_pthread_probe(name)
  "# This program uses Thread, so its runtime needs pthread from $(CC)'s target.\n" \
  "# Asked of $(CC) itself: a cross compiler for a target without pthread must\n" \
  "# say so here, not forty files in.\n" \
  "SP_PTHREAD := $(shell printf '\\043include <pthread.h>\\nstatic void *f(void *a){return a;}\\nint main(void){pthread_t t;return pthread_create(&t,0,f,0)!=0;}\\n' > .sp_pthread_probe.c && $(CC) -pthread .sp_pthread_probe.c -o .sp_pthread_probe >/dev/null 2>&1 && echo yes; rm -f .sp_pthread_probe.c .sp_pthread_probe)\n" \
  "ifneq ($(SP_PTHREAD),yes)\n" \
  "$(error #{name} uses Thread (Thread/Mutex/Queue/...) and $(CC) has no pthread: it cannot be built for this target)\n" \
  "endif\n"
end

def cmd_pack(prj, targets, outdir)
  bins = prj.bins
  spin_die("no bin/*.rb executables to pack") if bins.empty?
  targets = bins if targets.empty?
  spin_die("pack takes one executable at a time; got #{targets.length}") if targets.length > 1
  name = targets[0]
  spin_die("no such executable: bin/#{name}.rb") unless bins.include?(name)
  entry = File.join(prj.root, "bin", name + ".rb")

  outdir = File.join(prj.root, "build", "pack", name) if outdir == ""
  system("rm -rf #{outdir}")
  pack_mkdir(File.join(prj.root, "build"))
  pack_mkdir(File.join(prj.root, "build", "pack"))
  # the requested --out may be anywhere; make its parents the cheap way
  system("mkdir -p #{outdir}")
  pack_mkdir(File.join(outdir, "src"))
  pack_mkdir(File.join(outdir, "lib"))
  pack_mkdir(File.join(outdir, "lib", "regexp"))
  pack_mkdir(File.join(outdir, "lib", "regexp", "shim"))
  pack_mkdir(File.join(outdir, "lib", "regexp", "shim", "mruby"))
  pack_mkdir(File.join(outdir, "native"))

  # One compiler run writes the C and reports the build ingredients: `-c` and
  # `--print-build` compose. Two runs (the C, then the report) repeated the
  # whole front end for a few lines the first run already knew (#4456).
  flags = spin_flags(prj)
  cfile = File.join(outdir, "src", name + ".c")
  tmp = ENV["TMPDIR"].to_s
  tmp = "/tmp" if tmp == ""
  ccf = File.join(tmp, "spin-pack-#{Process.pid}.build")
  unless system("#{spinel_bin} #{entry} #{flags} -c --force --print-build -o #{cfile} > #{ccf}")
    system("rm -f #{ccf}")
    spin_die("pack: the compiler could not translate bin/#{name}.rb")
  end
  report = File.read(ccf)
  system("rm -f #{ccf}")

  # Read the report. One line per ingredient, `kind value`, so this is a split
  # on the first space and nothing more -- the previous shape of this was a
  # quote-aware shell splitter followed by a classifier that sorted tokens back
  # into the categories they had been assembled from, which is what a command
  # line costs when the consumer is a program.
  #
  # The runtime sources take the same defines and cflags as the generated TU:
  # a threaded program needs -DSP_THREADS when compiling the runtime as much as
  # when compiling its own translation unit, and the generated code writes its
  # own externs, so a mismatch there links and then misbehaves rather than
  # failing.
  cflags = ""
  libs = ""
  natives = ""
  rtdir = ""
  threaded = false   # the program uses Thread: its runtime needs pthread on the recipient
  pack_pkgs = []     # dep packages whose headers are already in the pack
  pkg_rules = ""     # one compile rule per dep package: its root is on -I
  report.each_line do |ln|
    ln = ln.chomp
    sp = ln.index(" ")
    next if sp.nil?
    kind = ln[0, sp]
    val = ln[(sp + 1)..-1].to_s
    next if val == ""
    if kind == "define" || kind == "cflag"
      cflags += " " + val
      threaded = true if val == "-DSP_THREADS"
    elsif kind == "lib"
      # -lcrypt is the one ingredient the compiler reports for ITS platform
      # rather than the program's: String#crypt is libc crypt(3), which glibc
      # ships as a separate library and Darwin folds into libSystem. A pack
      # made on a Mac would omit it and fail to link on Linux, so the Makefile
      # decides it from the recipient's uname instead (below).
      libs += " " + val unless val == "-lcrypt"
    elsif kind == "include"
      # every include pointed into the compiler's tree; the pack has its own
    elsif kind == "source"
      # the generated TU is src/<name>.c here
    elsif kind == "runtime"
      # The archive itself does not travel -- the runtime ships as source --
      # but the directory holding it is where that source lives, and asking the
      # compiler beats deriving it. This used to be <dir-of-spinel>/../lib,
      # computed from spinel_bin, which is the bare name "spinel" whenever the
      # compiler is not a sibling of spin (the PATH fallback). The arithmetic
      # then produced a directory that does not exist, every glob found nothing,
      # and `pack` reported success on a directory with no runtime in it: the
      # recipient got the first error, which is the wrong person to get it.
      rtdir = File.dirname(val)
    elsif kind == "link"
      # a package's native object. Its SOURCE is what travels; the object was
      # built for the packer's platform and the pack exists to leave that
      # behind. A `_mt` object is the threaded variant of one source file.
      parts = prj.native_source_parts(val)
      src = prj.native_source_for(val)
      if parts != ""
        # A dep's C travels in its own tree, as `spin build` compiles it:
        # the package's layout mirrored under native/<name>/ and compiled
        # with -I at its root. Flattening it lost every header outside the
        # source's own directory -- spinel-nokogiri's sources include
        # <libxml/...> from the package root -- and gave two packages'
        # `list.c` one object.
        pf = parts.split("\t")
        pdir = "native/" + pf[0]
        dst = File.join(outdir, pdir, pf[2] + ".c")
        mkdir_p_path(File.dirname(dst))
        pack_copy(src, dst)
        natives += " " + pdir + "/" + pf[2] + ".o"
        unless pack_pkgs.include?(pf[0])
          pack_pkgs.push(pf[0])
          # what a source includes: headers, and the `.inc` / `.def` tables
          # C code includes by the same rule (cmark's entities.inc)
          ["*.h", "*.inc", "*.def"].each do |pat|
            Dir.glob(File.join(pf[1], "**", pat)).each do |hf|
              hd = File.join(outdir, pdir, hf[(pf[1].length + 1)..-1].to_s)
              mkdir_p_path(File.dirname(hd))
              pack_copy(hf, hd)
            end
          end
          pkg_rules += pdir + "/%.o: " + pdir + "/%.c\n\t$(CC) $(CFLAGS) -I" + pdir + " -c $< -o $@\n\n"
        end
      elsif src != ""
        stem = File.basename(src)
        stem = stem[0, stem.length - 2]
        pack_copy(src, File.join(outdir, "native", stem + ".c"))
        natives += " native/" + stem + ".o"
        Dir.glob(File.join(File.dirname(src), "*.h")).each do |hf|
          pack_copy(hf, File.join(outdir, "native", File.basename(hf)))
        end
      else
        puts "pack: warning: no source for #{File.basename(val)}; the pack will not build without it"
      end
    end
  end

  spin_die("pack: the compiler did not report where its runtime lives") if rtdir == ""
  Dir.glob(File.join(rtdir, "*.c")).each { |f| pack_copy(f, File.join(outdir, "lib", File.basename(f))) }
  Dir.glob(File.join(rtdir, "*.h")).each { |f| pack_copy(f, File.join(outdir, "lib", File.basename(f))) }
  Dir.glob(File.join(rtdir, "regexp", "*.c")).each { |f| pack_copy(f, File.join(outdir, "lib", "regexp", File.basename(f))) }
  Dir.glob(File.join(rtdir, "regexp", "*.h")).each { |f| pack_copy(f, File.join(outdir, "lib", "regexp", File.basename(f))) }
  Dir.glob(File.join(rtdir, "regexp", "*.inc")).each { |f| pack_copy(f, File.join(outdir, "lib", "regexp", File.basename(f))) }
  # the headers the vendored mruby engine includes as <mruby.h> and
  # <mruby/string.h>: the directory shape is part of their names
  Dir.glob(File.join(rtdir, "regexp", "shim", "*.h")).each { |f| pack_copy(f, File.join(outdir, "lib", "regexp", "shim", File.basename(f))) }
  Dir.glob(File.join(rtdir, "regexp", "shim", "mruby", "*.h")).each { |f| pack_copy(f, File.join(outdir, "lib", "regexp", "shim", "mruby", File.basename(f))) }
  # lib/spinel/ is the ABI a native package compiles against -- it includes
  # <spinel/runtime.h> by that path, so the directory has to keep its name.
  if Dir.exist?(File.join(rtdir, "spinel"))
    pack_mkdir(File.join(outdir, "lib", "spinel"))
    Dir.glob(File.join(rtdir, "spinel", "*.h")).each do |f|
      pack_copy(f, File.join(outdir, "lib", "spinel", File.basename(f)))
    end
  end
  # A pack with no runtime source in it builds nowhere, and the only person who
  # would find out is the recipient. Refuse here instead.
  if Dir.glob(File.join(outdir, "lib", "*.c")).empty?
    spin_die("pack: no runtime sources under #{rtdir}")
  end

  mk = "# Generated by `spin pack`. Builds #{name} from C alone: a C compiler
"        "# and make, no spinel and no spin. Parallel-safe (`make -j`).
"        "#
"        "# CFLAGS below has two halves. The defines and the -I paths are what the
"        "# compiler REPORTED this program requires (`spinel --print-build`), and
"        "# changing them changes what gets built: a threaded program compiles its
"        "# runtime with -DSP_THREADS too, and the generated TU writes its own
"        "# externs, so a mismatch there links and then misbehaves rather than
"        "# failing. The -O2 is this Makefile's opinion and yours to change.
"        "#
"        "# CC is a default, not a decision. Cross-compiling is `make CC=<your cc>`
"        "# and the ingredients above travel unchanged -- which is the point of
"        "# them being ingredients rather than a command line. The compiler does
"        "# have to be GCC-compatible: the runtime headers use __attribute__
"        "# ((cleanup)), __COUNTER__, statement expressions and __typeof__, so
"        "# gcc and clang (including cross builds of them) work and a strict ISO C
"        "# compiler does not.
"        "CC ?= cc
"        "CFLAGS ?= -O2#{cflags} -Ilib -Ilib/regexp -Ilib/regexp/shim
"        "LIBS ?=#{libs}
"        "#{threaded ? pack_pthread_probe(name) : ""}"        "# String#crypt is libc crypt(3): a separate library on glibc, inside
"        "# libSystem on Darwin. The recipient's platform decides, not the packer's.
"        "ifneq ($(shell uname -s),Darwin)
"        "LIBS += -lcrypt
"        "endif
"        "
"        "RT := $(wildcard lib/*.c) $(wildcard lib/regexp/*.c)
"        "NATIVE :=#{natives}
"        "OBJS := src/#{name}.o $(RT:.c=.o) $(NATIVE)
"        "
"        "#{name}: $(OBJS)
"        "\t$(CC) $(CFLAGS) $(OBJS) $(LIBS) -o $@
"        "
"        "#{pkg_rules}%.o: %.c
"        "\t$(CC) $(CFLAGS) -c $< -o $@
"        "
"        "clean:
"        "\trm -f $(OBJS) #{name}
"        "
"        ".PHONY: clean
"
  File.write(File.join(outdir, "Makefile"), mk.gsub("\t", "	"))
  puts "pack #{name} -> #{outdir}"
  puts "  cd #{outdir} && make -j"
end

def cmd_build(prj, targets, extra)
  extra.split(" ").each do |a|
    if EMIT_ONLY_FLAGS.include?(a)
      spin_die("`#{a}` produces no executable, and `spin build` writes one to " \
               "build/bin/ -- run the compiler directly for that")
    end
  end
  bins = prj.bins
  spin_die("no bin/*.rb executables to build (a library is exercised via `spin test`)") if bins.empty?
  targets = bins if targets.empty?
  need = inputs_mtime(prj)
  # Declared native builds run first (vendor/ is excluded from the mtime
  # sweep, so a rebuilt artifact's own mtime is what re-triggers the link).
  prj.native_build_libs.split("\n").each do |l|
    next if l == ""
    m = File.mtime(l).to_i
    need = m if m > need
  end
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  bindir = File.join(prj.root, "build/bin")
  Dir.mkdir(bindir) unless Dir.exist?(bindir)
  targets.each do |t|
    spin_die("no such executable: bin/#{t}.rb") unless bins.include?(t)
    out = File.join(bindir, t)
    # The freshness check has to know which KIND of binary is sitting there.
    # `--debug` rides an env var rather than `extra`, so without this a debug
    # build right after a release one reports "up to date" and hands back the
    # release binary -- silently, which is the one outcome a debug flag must
    # never produce. A stamp beside the binary records the mode and a mismatch
    # forces a rebuild in either direction.
    mode = ENV["SPIN_DEBUG"].to_s != "" ? "debug" : "release"
    stamp = File.join(bindir, ".#{t}.mode")
    had = File.exist?(stamp) ? File.read(stamp).strip : "release"
    if File.exist?(out) && File.mtime(out).to_i > need && extra == "" && had == mode
      puts "build #{t} (up to date)"
      next
    end
    puts "build #{t}"
    compile(prj, File.join(prj.root, "bin/#{t}.rb"), out, extra)
    File.write(stamp, mode)
  end
end

def cmd_run(prj, args)
  target = ""
  run_args = []
  seen_dd = false
  args.each do |a|
    if a == "--"
      seen_dd = true
    elsif seen_dd
      run_args.push(a)
    elsif target == ""
      target = a
    end
  end
  bins = prj.bins
  if target == ""
    spin_die("no executables in bin/") if bins.empty?
    spin_die("multiple executables (#{bins.join(', ')}): spin run <name>") if bins.length > 1
    target = bins[0]
  end
  cmd_build(prj, [target], "")
  cmd = File.join(prj.root, "build/bin", target)
  run_args.each { |a| cmd += " #{a}" }
  ok = system(cmd)
  exit(ok ? 0 : 1)
end

# --- test --------------------------------------------------------------------

def cmd_test(prj, files, regen)
  tests = prj.tests
  spin_die("no test/*.rb files") if tests.empty?
  tests = files.map { |f| File.basename(f) } unless files.empty?
  tdir = File.join(prj.root, "build/test")
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  Dir.mkdir(tdir) unless Dir.exist?(tdir)
  inc = ""
  prj.dep_paths.each { |d| inc += " -I #{d}" }
  inc += " -I #{prj.root}"
  tests.each do |t|
    spin_die("no such test: test/#{t}") unless File.exist?(File.join(prj.root, "test", t))
  end
  # regen rewrites each snapshot from CRuby; kept serial (it only runs ruby).
  # Both streams, merged: that is what a run is compared against below, and
  # what the no-snapshot path already captures from CRuby. Taking stdout alone
  # here wrote a snapshot the very next `spin test` could not match, for any
  # program that writes to stderr (#3405).
  if regen
    tests.each do |t|
      src = File.join(prj.root, "test", t)
      system("ruby#{inc} #{src} > #{src}.expected 2>&1")
      puts "regen #{t}"
    end
    return
  end
  # Incremental-build freshness bound: a test binary newer than every project
  # input AND the compiler binary may be reused without recompiling (#3202).
  need = inputs_mtime(prj)
  cm = File.exist?(spinel_bin) ? File.mtime(spinel_bin).to_i : 0
  need = cm if cm > need
  bins = tests.map { |t| File.join(tdir, t[0..-4]) }
  # A binary newer than the freshness bound is still valid -- reuse it (marked
  # "(cached)") and skip the dominant recompile cost.
  cached = tests.map { |t| b = File.join(tdir, t[0..-4]); File.exist?(b) && File.mtime(b).to_i > need }
  # Phase 1: compile every stale test in PARALLEL. cc is the whole cost (the
  # spinel frontend is ~1ms) and the files are independent, so hand the compile
  # commands to `xargs -P` and let it saturate the cores -- ~ncores faster than
  # the old one-at-a-time build. Test binaries run once to check output, not for
  # speed, so compile at -O1 not the -O2 release default: -O1 still prunes the
  # unreferenced runtime-header statics (unlike -O0) while skipping the expensive
  # passes (~2x less cc per file). (#3202)
  cmds = []
  tests.each_index do |i|
    next if cached[i]
    # Drop the previous binary before compiling. A failed compile leaves it
    # where it was, and the run phase below reads File.exist? as "it built":
    # `spin test` printed the parse error, then ran the executable an OLDER
    # source had produced, compared ITS output against the snapshot, and
    # answered ok with exit 0 (#4085). What exists after this phase is now
    # what this phase produced.
    File.delete(bins[i]) if File.exist?(bins[i])
    cmds.push(compile_cmd(prj, File.join(prj.root, "test", tests[i]), bins[i], "-O 1"))
  end
  unless cmds.empty?
    # `nproc` is GNU coreutils; BSD/macOS answers via sysctl.
    nproc = `nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null`.to_i
    nproc = 1 if nproc < 1
    cf = File.join(tdir, ".compile-cmds")
    File.write(cf, cmds.join("\n") + "\n")
    # One compile command per input line. GNU xargs takes the delimiter
    # explicitly (-d); BSD xargs has no -d, but -I already reads one
    # line per command -- it just caps the substituted command at 255
    # bytes unless -S raises the limit (65522 is the documented max),
    # and GNU rejects -S. Pick the flag set by flavor.
    flavor = `xargs --version 2>/dev/null`.include?("GNU") ? "-d '\\n'" : "-S 65522"
    system("xargs -P #{nproc} #{flavor} -I CMD sh -c CMD < #{cf}")
    File.delete(cf) if File.exist?(cf)
  end
  # Phase 2: run each binary and compare, serial and in order (execution is
  # milliseconds; only the compile above was worth parallelizing).
  fails = 0
  tests.each_index do |i|
    t = tests[i]
    bin = bins[i]
    unless File.exist?(bin)
      puts "FAIL #{t} (build failed)"
      fails += 1
      next
    end
    exp = File.join(prj.root, "test", t) + ".expected"
    outf = bin + ".out"
    system("#{bin} > #{outf} 2>&1")
    actual = File.exist?(outf) ? File.read(outf) : ""
    if File.exist?(exp)
      expected = File.read(exp)
    else
      # no snapshot: diff directly against CRuby (the subset-parity check)
      cexp = bin + ".cruby"
      system("ruby#{inc} #{File.join(prj.root, "test", t)} > #{cexp} 2>&1")
      expected = File.exist?(cexp) ? File.read(cexp) : ""
    end
    if actual == expected
      puts "ok   #{t}" + (cached[i] ? " (cached)" : "")
    else
      puts "FAIL #{t}"
      puts "--- expected"
      print expected
      puts "--- actual"
      print actual
      fails += 1
    end
  end
  puts "#{tests.length - fails}/#{tests.length} passed"
  exit 1 if fails > 0
end


# --- add / lock / fetch / vendor (M1) ------------------------------------------

def lock_from_records(prj)
  lines = []
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    lines.push("\n[lock." + f[0] + "]\nversion = \"" + f[2] + "\"\n")
    if f[3] == "git" || f[3] == "index"
      us = f[4].split("\x01")
      lines.push("git = \"" + us[0] + "\"\nref = \"" + us[1] + "\"\n")
    else
      lines.push("path = \"" + f[4] + "\"\n")
    end
  end
  write_lock(prj.root, lines)
  puts "locked " + prj.dep_paths.length.to_s + " package(s)"
end

def jq_str(s)
  out = ""
  s.split("").each do |ch|
    if ch == "\\" || ch == "\""
      out += "\\" + ch
    else
      out += ch
    end
  end
  "\"" + out + "\""
end

def cmd_list(prj, json)
  if json
    out = "["
    first = true
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      out += "," unless first
      first = false
      out += "{\"name\":" + jq_str(f[0]) + ",\"version\":" + jq_str(f[2]) +
             ",\"kind\":" + jq_str(f[3]) + ",\"source\":" + jq_str(src) + "}"
    end
    puts out + "]"
  else
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      puts f[0] + " " + f[2] + " (" + f[3] + " " + src + ")"
    end
  end
end

# name -> resolved dir, for walking each package's own manifest
def tree_children(dir)
  mf = File.join(dir, "spin.toml")
  return [] unless File.exist?(mf)
  TomlDoc.parse(File.read(mf)).table_keys("dependencies")
end

def tree_walk(prj, name, dir, version, indent, seen, json)
  out = ""
  if json
    out = "{\"name\":" + jq_str(name) + ",\"version\":" + jq_str(version) + ",\"deps\":["
  else
    puts indent + name + " " + version
  end
  first = true
  tree_children(dir).each do |dep|
    # resolved location/version from the flat record set
    ddir = ""
    dver = ""
    prj.dep_records.split("\n").each do |rec|
      f = rec.split("\t")
      if f[0] == dep
        ddir = f[1]
        dver = f[2]
      end
    end
    next if ddir == ""
    if seen.include?("|" + dep + "|")
      puts indent + "  " + dep + " " + dver + " (...)" unless json
      next
    end
    sub = tree_walk(prj, dep, ddir, dver, indent + "  ", seen + "|" + dep + "|", json)
    if json
      out += "," unless first
      first = false
      out += sub
    end
  end
  json ? out + "]}" : ""
end

def cmd_tree(prj, json)
  r = tree_walk(prj, prj.name, prj.root, gem_version_of(prj.root), "", "|" + prj.name + "|", json)
  puts r if json
end

def cmd_add(root, name, url, ref, pth, cons, feats)
  spin_die("usage: spin add <name> [--version C | --git URL [--ref R] | --path DIR] [--features A,B]") if name == ""
  mf = File.join(root, "spin.toml")
  text = File.read(mf)
  flist = ""
  feats.split(",").each do |ft|
    next if ft.strip == ""
    flist += ", " unless flist == ""
    flist += "\"" + ft.strip + "\""
  end
  fmember = flist == "" ? "" : ", features = [" + flist + "]"
  spec = ""
  if pth != ""
    spec = "{ path = \"" + pth + "\"" + fmember + " }"
  elsif url != ""
    spec = ref == "" ? "{ git = \"" + url + "\"" + fmember + " }"
                     : "{ git = \"" + url + "\", ref = \"" + ref + "\"" + fmember + " }"
  else
    # index form: bare name takes any release, --version narrows it; with
    # features the constraint moves inline as `version`
    index_refresh(false)
    c2 = cons == "" ? "*" : cons
    spec = flist == "" ? "\"" + c2 + "\""
                       : "{ version = \"" + c2 + "\"" + fmember + " }"
  end
  line = name + " = " + spec + "\n"
  if text.include?("\n[dependencies]\n")
    text = text.sub("\n[dependencies]\n", "\n[dependencies]\n" + line)
  elsif text.start_with?("[dependencies]\n")
    text = "[dependencies]\n" + line + text[15..-1]
  else
    text += "\n[dependencies]\n" + line
  end
  File.write(mf, text)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "added " + name
end

def cmd_search(term)
  index_refresh(false)
  d = File.join(index_dir(false), "packages")
  found = 0
  Dir.glob(d + "/*.toml").sort.each do |gf|
    nm = File.basename(gf)[0..-6]
    next unless term == "" || nm.include?(term)
    gdoc = TomlDoc.parse(File.read(gf))
    best = ""
    i = 0
    while i < gdoc.array_len("release")
      v = gdoc.get("release." + i.to_s, "version")
      best = v if best == "" || vcmp(v, best) > 0
      i += 1
    end
    puts nm + " " + best + " " + gdoc.get("", "repo")
    found += 1
  end
  puts "no matches" if found == 0
end

def cmd_remove(root, name)
  mf = File.join(root, "spin.toml")
  out = ""
  File.read(mf).split("\n").each do |l|
    out += l + "\n" unless l.strip.start_with?(name + " ") || l.strip.start_with?(name + "=")
  end
  File.write(mf, out)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "removed " + name
end

# --- publish (index PR automation) --------------------------------------------
# `spin publish` folds "get my release into the index" into one command:
# validate identity + a pushed, version-consistent commit, run the tests as a
# hard gate (R8), write packages/<name>.toml, then submit -- straight push with
# --direct (index write access), a gh-driven fork + PR when gh is available,
# or printed instructions otherwise. No tarballs, no accounts: the git
# identity is the identity, and nothing executes at fetch time.

def publish_repo_url(root, override)
  u = override
  u = sh_read("git -C " + root + " remote get-url origin") if u == ""
  spin_die("publish: no git remote (set one or pass --repo URL)") if u == ""
  # normalize the GitHub ssh form; consumers clone anonymously
  if u.start_with?("git@github.com:")
    u = "https://github.com/" + u[15..-1].to_s
  end
  u = u[0..-5] if u.end_with?(".git")
  spin_die("publish: " + u + " is not fetchable by others (file:// and local paths cannot be published)") if u.start_with?("file://") || u.start_with?("/") || u.start_with?(".")
  u
end

# `spin install`: build this package's executables and copy them onto PATH --
# the last step of "I wrote a CLI and now I use it". Local sources only;
# installing a tool from the index is a separate (deferred) verb, so the
# rubygems reading of `install <name>` never collides with bin/<name>.
def install_dir(prefix)
  return prefix if prefix != ""
  d = ENV["XDG_BIN_HOME"].to_s
  return d if d != ""
  File.join(ENV["HOME"].to_s, ".local/bin")
end

def cmd_install(prj, targets, prefix, uninstall)
  bins = prj.bins
  spin_die("no bin/*.rb executables (a library has nothing to install)") if bins.empty?
  targets = bins if targets.empty?
  targets.each { |t| spin_die("no such executable: bin/#{t}.rb") unless bins.include?(t) }
  d = install_dir(prefix)
  if uninstall
    targets.each do |t|
      f = File.join(d, t)
      if File.exist?(f)
        File.delete(f)
        puts "uninstalled " + f
      else
        puts "not installed: " + f
      end
    end
    return
  end
  cmd_build(prj, targets, "")
  system("mkdir -p " + d)
  targets.each do |t|
    src = File.join(prj.root, "build/bin", t)
    dst = File.join(d, t)
    spin_die("install: copy failed for " + t) unless system("install -m 755 " + src + " " + dst)
    puts "installed " + t + " -> " + dst
  end
end

def cmd_publish(root, repo_override, ref_override, direct)
  toml = TomlDoc.parse(File.read(File.join(root, "spin.toml")))
  name = toml.get("package", "name")
  version = toml.get("package", "version")
  spin_die("publish makes identity mandatory: set [package] name and version in spin.toml") if name == "" || version == ""
  repo = publish_repo_url(root, repo_override)

  dirty = sh_read("git -C " + root + " status --porcelain")
  spin_die("publish: uncommitted changes (commit and push first)") if dirty != "" && ref_override == ""
  ref = ref_override == "" ? sh_read("git -C " + root + " rev-parse HEAD") : ref_override
  spin_die("publish: cannot resolve HEAD (is this a git repo?)") if ref == ""

  # the commit must be reachable by consumers: some remote branch contains it
  reach = sh_read("git -C " + root + " branch -r --contains " + ref)
  spin_die("publish: commit " + ref[0..11].to_s + " is not on any remote branch (git push first)") if reach == ""

  # the tree at the release ref must carry the version being published
  tv = ""
  sh_read("git -C " + root + " show " + ref + ":spin.toml").split("\n").each do |l|
    tv = TomlDoc.parse(l + "\n").get("", "version") if l.strip.start_with?("version")
  end
  spin_die("publish: spin.toml at " + ref[0..11].to_s + " says version \"" + tv + "\", manifest says \"" + version + "\"") if tv != version

  # hard test gate (R8): a package publishes with passing tests or not at all
  prj = Project.new(root)
  spin_die("publish requires tests: add test/*.rb (spin test)") if prj.tests.empty?
  cmd_test(prj, [], false)   # exits non-zero on any failure

  # write the index entry
  index_refresh(false)
  idir = index_dir(false)
  gf = File.join(idir, "packages", name + ".toml")
  entry = ""
  if File.exist?(gf)
    gdoc = TomlDoc.parse(File.read(gf))
    erepo = gdoc.get("", "repo")
    spin_die("publish: index name \"" + name + "\" belongs to " + erepo + " (same name means the same library; rename per the name policy)") if erepo != repo
    i = 0
    while i < gdoc.array_len("release")
      # Compare the way the SOLVER does, not textually. vcmp pads the shorter
      # side with zeros and reads each field as a number, so "1.2" and "1.2.0"
      # -- and "2026.9.8" and "2026.09.08" -- are ONE version to every
      # constraint, while a string compare lets both into the index. The
      # duplicate then also splits the native object cache, whose directory
      # name is the version as written.
      ev = gdoc.get("release." + i.to_s, "version")
      spin_die("publish: " + name + " " + version + " is already in the index" +
               (ev == version ? "" : " (as \"" + ev + "\", which is the same version)")) if vcmp(ev, version) == 0
      i += 1
    end
    entry = File.read(gf)
  else
    entry = "name = \"" + name + "\"\nrepo = \"" + repo + "\"\n"
  end
  entry += "\n[[release]]\nversion = \"" + version + "\"\nref = \"" + ref + "\"\n"
  rev = spinel_rev
  if rev != ""
    entry += "\n[[probe]]\nversion = \"" + version + "\"\nspinel = \"" + rev + "\"\nresult = \"pass\"\ndate = \"" + Time.now.strftime("%Y-%m-%d") + "\"\n"
  end

  if direct
    File.write(gf, entry)
    ok = system("git -C " + idir + " add packages/" + name + ".toml") &&
         system("git -C " + idir + " -c user.email=spin@publish -c user.name=spin commit -qm \"" + name + " " + version + "\"") &&
         system("git -C " + idir + " push -q origin HEAD")
    spin_die("publish --direct: push to the index failed (no write access?)") unless ok
    puts "published " + name + " " + version + " (direct)"
    return
  end

  if system("gh --version > /dev/null 2>&1")
    # work in a scratch clone so the cache index stays on main
    tmp = File.join(File.dirname(idir), ".publish-" + name)
    system("rm -rf " + tmp)
    spin_die("publish: cannot clone the index") unless system("git clone -q " + idir + " " + tmp)
    File.write(File.join(tmp, "packages", name + ".toml"), entry)
    br = "publish-" + name + "-" + version.gsub(".", "-")
    login = sh_read("gh api user -q .login")
    spin_die("publish: gh is installed but not authenticated (gh auth login)") if login == ""
    system("gh repo fork " + spin_index_url + " --clone=false > /dev/null 2>&1")
    ok = system("git -C " + tmp + " checkout -qb " + br) &&
         system("git -C " + tmp + " add packages/" + name + ".toml") &&
         system("git -C " + tmp + " -c user.email=spin@publish -c user.name=spin commit -qm \"" + name + " " + version + "\"") &&
         system("git -C " + tmp + " push -q https://github.com/" + login + "/spin-index.git " + br + ":" + br)
    spin_die("publish: pushing the fork branch failed") unless ok
    body = "spin publish: " + name + " " + version + "%0A%0Arepo: " + repo + "%0Aref: " + ref + "%0Atests: pass (spin test gate)"
    body = body.gsub("%0A", "\n")
    okpr = system("gh pr create --repo " + spin_index_url.sub("https://github.com/", "") +
                  " --head " + login + ":" + br +
                  " --title \"" + name + " " + version + "\"" +
                  " --body \"" + body + "\"")
    system("rm -rf " + tmp)
    spin_die("publish: gh pr create failed") unless okpr
    puts "published " + name + " " + version + " (PR opened)"
    return
  end

  puts "gh not found -- open a pull request against " + spin_index_url
  puts "adding this as packages/" + name + ".toml:"
  puts ""
  puts entry
end

def cmd_vendor(prj)
  vg = File.join(prj.root, "vendor")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  vg = File.join(vg, "packages")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    dst = File.join(vg, f[0] + "-" + f[2])
    system("rm -rf " + dst)
    system("cp -a " + f[1] + " " + dst)
    puts "vendored " + f[0] + "-" + f[2]
  end
end

# --- scaffold ----------------------------------------------------------------

APP_MANIFEST = <<TOML
# spin manifest -- an application needs no [package] identity.
# Add dependencies like:
#   [dependencies]
#   ansi = { path = "../spinel-ansi" }
TOML


# ---- spin ext: compile a Ruby kernel into a CRuby extension ---------------
# (docs/internals/ext-design.md M2.) `new` scaffolds a gem whose source is
# plain Ruby (the fallback and the oracle), `build` emits the kernel, shim
# and header via `spinel --ext cruby` and vendors the runtime sources flat
# into ext/, `test` compiles the extension and runs the differential
# harness -- every case through the pure kernel AND the .so.

def ext_camel(name)
  out = ""
  name.split("_").each { |w| out += w.length > 0 ? w[0, 1].upcase + w[1, w.length] : "" }
  out
end

# [ext] module + entries from spin.toml; dies with usage when absent.
def ext_manifest(root)
  toml = TomlDoc.parse(File.read(File.join(root, "spin.toml")))
  mod = toml.get("ext", "module")
  entries = toml.get_array("ext", "entries")
  spin_die("spin.toml has no [ext] section (module = \"...\", entries = [\"Mod.m\"])") if mod == "" || entries == ""
  [mod, entries]
end

# The spinel installation's runtime sources (bin/spinel -> ../lib).
def ext_runtime_dir
  rt = spinel_hdr_dir
  if rt == "" || Dir.glob(File.join(rt, "*.c")).empty?
    spin_die("spin ext build: can't find the Spinel runtime sources; set SPINEL_HDR_DIR to the runtime's lib directory")
  end
  rt
end

def cmd_ext_new(name)
  spin_die("usage: spin ext new <name>") if name == ""
  spin_die("#{name}: already exists") if File.exist?(name)
  mod = ext_camel(name)
  Dir.mkdir(name)
  Dir.mkdir(File.join(name, "lib"))
  Dir.mkdir(File.join(name, "lib", name))
  Dir.mkdir(File.join(name, "ext"))
  Dir.mkdir(File.join(name, "ext", name))
  Dir.mkdir(File.join(name, "test"))
  File.write(File.join(name, "spin.toml"), <<TOML)
[package]
name = "#{name}"
version = "0.1.0"

[ext]
module = "#{mod}"
entries = ["#{mod}.double"]
TOML
  File.write(File.join(name, "lib", "#{name}.rb"), <<RB)
# #{name}: loads the compiled extension, or the same code as plain Ruby.
begin
  require "#{name}/#{name}"
rescue LoadError
  require "#{name}/kernel"
end
RB
  File.write(File.join(name, "lib", name, "kernel.rb"), <<RB)
# The kernel: plain Ruby. Runs under CRuby unchanged (the fallback and the
# test oracle); `spin ext build` compiles it into the extension. The guarded
# block below is the manual test driver AND the entry methods' call-site
# type source -- it never runs at extension load.
module #{mod}
  def self.double(n)
    n * 2
  end
end

if __FILE__ == $0
  p #{mod}.double(21)
end
RB
  File.write(File.join(name, "ext", name, "extconf.rb"), <<RB)
require "mkmf"
$INCFLAGS << " -I$(srcdir)"
create_makefile("#{name}/#{name}")
RB
  File.write(File.join(name, "test", "differential.rb"), <<RB)
# Every case runs through the pure-Ruby kernel AND the compiled extension,
# and the answers must match -- the kernel is its own oracle. Run via
# `spin ext test`; add cases as [:method, [args...]].
so_dir = ENV["SPIN_EXT_SO_DIR"].to_s
raise "run this via `spin ext test`" if so_dir.empty?
$LOAD_PATH.unshift(so_dir)
# The compiled extension itself, not the loader: the loader falls back to the
# kernel, and the kernel against itself always matches.
require "#{name}/#{name}"

pure = Module.new
src = File.read(File.expand_path("../lib/#{name}/kernel.rb", __dir__))
pure.module_eval(src.sub(/^if __FILE__.*\z/m, ""))
PURE = pure::#{mod}

CASES = [
  [:double, [21]],
  [:double, [0]],
  [:double, [-7]],
]

fails = 0
CASES.each do |m, args|
  want = PURE.public_send(m, *args)
  got = #{mod}.public_send(m, *args)
  next if want == got
  fails += 1
  puts "DIFF \#{m}(\#{args.map(&:inspect).join(", ")}): pure=\#{want.inspect} ext=\#{got.inspect}"
end
puts fails.zero? ? "differential: \#{CASES.length}/\#{CASES.length} match" : "differential: \#{fails} case(s) diverge"
exit(fails.zero? ? 0 : 1)
RB
  author = sh_read("git config user.name").strip
  author = "unknown" if author == ""
  File.write(File.join(name, "#{name}.gemspec"), <<RB)
Gem::Specification.new do |s|
  s.name = "#{name}"
  s.version = "0.1.0"
  s.summary = "#{name}: a Ruby kernel compiled to a native extension by Spinel"
  s.authors = ["#{author}"]
  s.files = Dir["lib/**/*.rb", "ext/**/*.{c,h,rb}"]
  s.extensions = ["ext/#{name}/extconf.rb"]
  s.required_ruby_version = ">= 3.0"
end
RB
  File.write(File.join(name, ".gitignore"), "/build/\n/ext/#{name}/*.o\n/ext/#{name}/Makefile\n")
  system("git -C #{name} init -q")
  puts "created #{name}/ (extension gem; edit lib/#{name}/kernel.rb, then `spin ext build`)"
end

def cmd_ext_build(root)
  pkg = TomlDoc.parse(File.read(File.join(root, "spin.toml"))).get("package", "name")
  spin_die("spin.toml has no [package] name") if pkg == ""
  parts = ext_manifest(root)
  entries = parts[1].split("\n").join(",")
  extdir = File.join(root, "ext", pkg)
  kernel = File.join(root, "lib", pkg, "kernel.rb")
  spin_die("#{kernel}: not found") unless File.exist?(kernel)
  Dir.mkdir(extdir) unless File.exist?(extdir)
  cmd = spinel_bin + " " + kernel + " -c --no-line-map --ext cruby" +
        " --ext-init spx_init_" + pkg +
        " --ext-entry " + entries +
        " -o " + File.join(extdir, pkg + ".c")
  spin_die("spin ext build: spinel failed") unless run_command(cmd)
  # vendor the runtime sources flat beside the generated C (basenames are
  # unique across lib/ and lib/regexp/); the gem then builds with cc alone.
  rt = ext_runtime_dir
  n = 0
  (Dir.glob(File.join(rt, "*.c")) + Dir.glob(File.join(rt, "*.h")) +
   Dir.glob(File.join(rt, "regexp", "*.c")) + Dir.glob(File.join(rt, "regexp", "*.h")) +
   Dir.glob(File.join(rt, "regexp", "*.inc")) + Dir.glob(File.join(rt, "regexp", "shim", "*.h"))).each do |f|
    File.write(File.join(extdir, File.basename(f)), File.read(f))
    n += 1
  end
  # <mruby/string.h> and its siblings keep their directory: the vendored
  # regexp engine names them by that path
  mdir = File.join(extdir, "mruby")
  Dir.mkdir(mdir) unless File.exist?(mdir)
  Dir.glob(File.join(rt, "regexp", "shim", "mruby", "*.h")).each do |f|
    File.write(File.join(mdir, File.basename(f)), File.read(f))
    n += 1
  end
  unless File.exist?(File.join(extdir, "spinel_rt.h")) && File.exist?(File.join(extdir, "sp_gc.c"))
    spin_die("spin ext build: no runtime sources in #{rt}")
  end
  puts "built ext/#{pkg}/ (#{pkg}.c, #{pkg}.h, #{pkg}_ext.c + #{n} runtime files)"
end

def cmd_ext_test(root)
  pkg = TomlDoc.parse(File.read(File.join(root, "spin.toml"))).get("package", "name")
  extdir = File.join(root, "ext", pkg)
  spin_die("run `spin ext build` first") unless File.exist?(File.join(extdir, pkg + "_ext.c"))
  rh = sh_read("ruby -e 'puts RbConfig::CONFIG[\"rubyhdrdir\"]'").strip
  ra = sh_read("ruby -e 'puts RbConfig::CONFIG[\"rubyarchhdrdir\"]'").strip
  dlext = sh_read("ruby -e 'puts RbConfig::CONFIG[\"DLEXT\"]'").strip
  spin_die("spin ext test: needs the ruby development headers (ruby.h)") if rh == "" || !File.exist?(File.join(rh, "ruby.h"))
  so_dir = File.join(root, "build", "ext")
  Dir.mkdir(File.join(root, "build")) unless File.exist?(File.join(root, "build"))
  Dir.mkdir(so_dir) unless File.exist?(so_dir)
  pkg_dir = File.join(so_dir, pkg)
  Dir.mkdir(pkg_dir) unless File.exist?(pkg_dir)
  soflags = sh_read("uname -s").strip == "Darwin" ? "-bundle -Wl,-undefined,dynamic_lookup" : "-shared"
  cc = ENV["CC"].to_s == "" ? "cc" : ENV["CC"]
  cmd = cc + " " + soflags + " -fPIC -O2 -Wno-all" +
        " -I" + rh + " -I" + ra + " -I" + extdir +
        " " + Dir.glob(File.join(extdir, "*.c")).join(" ") +
        " -lm -o " + File.join(pkg_dir, pkg + "." + dlext)
  spin_die("spin ext test: extension did not compile") unless run_command(cmd)
  # The .so sits where extconf.rb installs it (<pkg>/<pkg>), so the loader in
  # lib/ requires it as it would from the installed gem.
  ok = system("SPIN_EXT_SO_DIR=" + so_dir + " ruby -I " + File.join(root, "lib") +
              " " + File.join(root, "test", "differential.rb"))
  spin_die("spin ext test: differential failed") unless ok
end

def cmd_new(name, lib)
  spin_die("usage: spin new <name> [--lib]") if name == ""
  spin_die("#{name}: already exists") if File.exist?(name)
  Dir.mkdir(name)
  Dir.mkdir(File.join(name, "test"))
  if lib
    File.write(File.join(name, "spin.toml"),
               "[package]\nname = \"#{name}\"\nversion = \"0.1.0\"\n\n# published repos are conventionally named spinel-#{name}\n")
    File.write(File.join(name, "#{name}.rb"), "# #{name}: library entry (require \"#{name}\")\n")
  else
    File.write(File.join(name, "spin.toml"), APP_MANIFEST)
    Dir.mkdir(File.join(name, "bin"))
    File.write(File.join(name, "bin/#{name}.rb"), "puts \"Hello from #{name}\"\n")
  end
  File.write(File.join(name, ".gitignore"), "/build/\n")
  system("git -C #{name} init -q")
  puts "created #{name}/#{lib ? " (library)" : ""}"
end

def cmd_init
  spin_die("spin.toml already exists") if File.exist?("spin.toml")
  File.write("spin.toml", APP_MANIFEST)
  puts "wrote spin.toml"
end

# --- main --------------------------------------------------------------------

# --verbose belongs to spin and only to spin: strip it from the part of
# ARGV before the `--` separator, leaving any --verbose after `--` in
# `rest` so it reaches the program (e.g. `spin run app -- --verbose`
# forwards --verbose to the app, not to spin).
dd = ARGV.index("--")
spin_part = dd ? ARGV[0, dd] : ARGV
app_part = dd ? ARGV[(dd + 1)..] : []
$spin_verbose = spin_part.count("--verbose") > 0 || ENV["SPIN_VERBOSE"].to_s != ""
args = spin_part.reject { |a| a == "--verbose" } + (dd ? ["--"] : []) + app_part
cmd = args.empty? ? "" : args[0]
rest = args[1..] || []
# `spin <command> --help` answers with the usage and runs nothing: a command
# like pack or build starts work at once otherwise, which is not what someone
# asking for help wanted (#4507). Only the part before `--` is spin's.
if (rest.index("--") ? rest[0, rest.index("--")] : rest).any? { |a| a == "--help" || a == "-h" }
  puts SPIN_USAGE
  exit 0
end

case cmd
when "new"
  lib = rest.include?("--lib")
  names = rest.reject { |a| a.start_with?("--") }
  cmd_new(names.empty? ? "" : names[0], lib)
when "init"
  cmd_init
when "add"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  nm = ""
  url = ""
  ref = ""
  pth = ""
  cons = ""
  feats = ""
  i2 = 0
  while i2 < rest.length
    a = rest[i2]
    if a == "--git"
      i2 += 1
      url = rest[i2].to_s
    elsif a == "--ref"
      i2 += 1
      ref = rest[i2].to_s
    elsif a == "--path"
      i2 += 1
      pth = rest[i2].to_s
    elsif a == "--version"
      i2 += 1
      cons = rest[i2].to_s
    elsif a == "--features"
      i2 += 1
      feats = rest[i2].to_s
    elsif !a.start_with?("--") && nm == ""
      nm = a
    end
    i2 += 1
  end
  cmd_add(root, nm, url, ref, pth, cons, feats)
when "remove"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  cmd_remove(root, rest.empty? ? "" : rest[0])
when "lock", "fetch", "vendor"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  lock_from_records(prj) if cmd == "lock"
  puts "fetched " + prj.dep_paths.length.to_s + " package(s)" if cmd == "fetch"
  cmd_vendor(prj) if cmd == "vendor"
when "ext"
  sub = rest.empty? ? "" : rest[0]
  if sub == "new"
    cmd_ext_new(rest.length > 1 ? rest[1] : "")
  elsif sub == "build" || sub == "test"
    root = find_root(Dir.pwd)
    spin_die("no spin.toml found") if root == ""
    cmd_ext_build(root) if sub == "build"
    cmd_ext_test(root) if sub == "test"
  else
    spin_die("usage: spin ext new <name> | spin ext build | spin ext test")
  end
when "flags"
  # The handoff: spin resolves the dependencies and warms the native cache,
  # then hands the compiler flags to whoever is driving the build. An
  # application whose repository spin does not own -- Ruby and C side by side
  # under one Makefile -- keeps its layout and still consumes packages (#4105).
  #
  # `--deps` prints the toolchain files that build instead of the flags, for a
  # Makefile to list as prerequisites. Without them a rule whose prerequisites
  # are the .rb sources answers "up to date" after the compiler or its runtime
  # changed, and the next thing you run is a binary built by the old one -- an
  # A/B measurement compared two builds of the same compiler that way, and the
  # only tell was that a flag it had armed produced no output (#4386). The
  # runtime archives are listed as well as the compiler: a change under lib/
  # rebuilds them and leaves bin/spinel untouched.
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  if rest.include?("--deps")
    puts toolchain_deps.join(" ")
  else
    puts spin_flags(Project.new(root))
  end
when "pack"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  outdir = ""
  tg = []
  i = 0
  while i < rest.length
    if rest[i] == "--out"
      outdir = i + 1 < rest.length ? File.expand_path(rest[i + 1]) : ""
      i += 1
    else
      tg << rest[i]
    end
    i += 1
  end
  cmd_pack(Project.new(root), tg, outdir)
when "search"
  cmd_search(rest.empty? ? "" : rest[0])
when "install"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  pfx = ""
  names = []
  i4 = 0
  while i4 < rest.length
    a4 = rest[i4]
    if a4 == "--prefix"
      i4 += 1
      pfx = rest[i4].to_s
    elsif !a4.start_with?("--")
      names.push(a4)
    end
    i4 += 1
  end
  cmd_install(prj, names, pfx, rest.include?("--uninstall"))
when "publish"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  rp = ""
  rf = ""
  i3 = 0
  while i3 < rest.length
    a3 = rest[i3]
    if a3 == "--repo"
      i3 += 1
      rp = rest[i3].to_s
    elsif a3 == "--ref"
      i3 += 1
      rf = rest[i3].to_s
    end
    i3 += 1
  end
  cmd_publish(root, rp, rf, rest.include?("--direct"))
when "list", "tree"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  json = rest.include?("--json")
  cmd_list(prj, json) if cmd == "list"
  cmd_tree(prj, json) if cmd == "tree"
when "trust"
  spin_die("usage: spin trust <name>") if rest.empty?
  native_trust!(rest[0])
  puts "trusted: #{rest[0]} (native build steps run without prompting)"
when "build", "run", "test", "clean"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found (run `spin init`, or `spin new <name>`)") if root == ""
  if rest.include?("--allow-native-build")
    ENV["SPIN_ALLOW_NATIVE_BUILD"] = "1"
    rest = rest.reject { |a| a == "--allow-native-build" }
  end
  # Only args BEFORE a `--` are spin's; everything after belongs to the target
  # program. `-g` is a plausible flag for a program to want, and `spin run app
  # -- -g` must pass it through rather than turn into a debug build.
  dd = rest.index("--")
  spin_args = dd ? rest[0, dd] : rest
  if spin_args.include?("--debug") || spin_args.include?("-g")
    ENV["SPIN_DEBUG"] = "1"
    tail = dd ? rest[dd..] : []
    rest = spin_args.reject { |a| a == "--debug" || a == "-g" } + tail
  end
  prj = Project.new(root)
  case cmd
  when "build"
    extra = ""
    if rest.include?("--")
      di = rest.index("--").to_i
      extra = rest[(di + 1)..-1].join(" ")
      # `rest[0, di]`, not `rest[0..(di - 1)]`: with `--` first (`spin build --
      # --profile`, no target named) di is 0 and `rest[0..-1]` is the WHOLE
      # array, so `--` survives as a build target and the run dies with
      # "no such executable: bin/--.rb" instead of building every bin with the
      # extra flag.
      rest = rest[0, di]
    end
    cmd_build(prj, rest, extra)
  when "run"
    cmd_run(prj, rest)
  when "test"
    regen = rest.include?("--regen")
    files = rest.reject { |a| a.start_with?("--") }
    cmd_test(prj, files, regen)
  when "clean"
    run_command("rm -rf #{File.join(prj.root, 'build')}")
    puts "cleaned"
  end
when "--version"
  # spin and spinel ship together and normally carry one stamp, so one line
  # in spinel's shape, with the C compiler the driven spinel reports:
  #   spin 2026.09.12 (112bae85) [gcc 13.3.0 (cc)]
  # The spinel spin drives is the sibling binary when spin is installed
  # beside one and otherwise the one on PATH, so it CAN be another build;
  # only then is it named, on a second line.
  mine = "spin #{SPIN_RELEASE} (#{SPIN_BUILD_REV})"
  sv = sh_read(spinel_bin + " --version").strip
  f = sv.split(" ")
  same = f.length >= 3 && f[1] == SPIN_RELEASE && f[2] == "(#{SPIN_BUILD_REV})"
  cc = sv.index("[") ? sv[sv.index("[")..-1] : ""
  if same
    puts cc == "" ? mine : mine + " " + cc
  else
    puts mine
    puts "driving " + sv if sv != ""
  end
else
  puts SPIN_USAGE
  exit(cmd == "" || cmd == "help" || cmd == "--help" ? 0 : 3)
end
