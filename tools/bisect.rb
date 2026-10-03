# spinel-bisect: name the compiler decisions behind a wrong answer.
#
# The compiler gives each optimization that would be a miscompile if its
# check were wrong a key, `kind@site` (src/decide.c), and SPINEL_DECISIONS
# restricts a compile to the keys in a file. This tool builds the program
# under subsets of the keys it logged, asks an oracle whether each build is
# right, and prints the smallest set of decisions it finds that makes the
# program wrong on its own: which optimization, and where in the source.
#
# Usage: spinel bisect FILE.rb [options] [-- COMPILER-FLAGS...]
#
#   --expected FILE   right means: stdout is FILE's contents and the exit
#                     status is 0
#   --cruby           right means: `spinel diff` finds no difference from
#                     CRuby (takes no compiler flags)
#   --oracle-cmd CMD  right means: CMD exits 0. `{}` in CMD is the binary
#                     built under the subset; a CMD without `{}` is run as it
#                     is, with SPINEL_DECISIONS set, and does its own build.
#                     As for `git bisect run`: 0 right, 125 cannot tell,
#                     anything else wrong
#   --timeout SEC     per-run time limit (default 30): a program past it is
#                     wrong, an oracle command past it could not tell
#   --keep-tmp        leave the scratch files, among them the allow-lists
#
# With no oracle option the reference is the program itself with every keyed
# decision denied: the tool names the decisions that change what it prints
# or how it exits, and needs to be told nothing about what is right.
#
# Exit status (a contract):
#   0  localized: the decisions are printed, one `key ...` line each
#   1  no keyed decision changes the answer (the bug is elsewhere)
#   2  nothing to bisect: the unrestricted build is right
#   3  inconclusive (the subsets that would decide it do not build or
#      cannot be judged, or the program does not do the same twice)
#   4  the tool's own error (no file, no compiler, a bad option)
#
# The environment passes through, so `SPINEL_GC_STRESS=1 spinel bisect ...`
# bisects a collection-timing failure. SPINEL_DECISIONS and
# SPINEL_DECISIONS_LOG are the tool's own to set and do not.
#
# Written in the spinel subset and compiled by spinel (see tools/README.md).

require_relative "tool_common"
require_relative "bisect_search"

$bisect_timeout = 30

# The process group of the run in flight, or 0. A run is the leader of a
# group of its own, so that the time limit ends what it started as well;
# the terminal's interrupt therefore does not reach it, and main's trap
# sends it on.
$bisect_running = 0

# The scratch files of this run, for whichever way it ends.
$bisect_scratch = []

def bisect_cleanup
  $bisect_scratch.each do |f|
    begin
      File.delete(f) if File.exist?(f)
    rescue StandardError
      nil
    end
  end
end

# The tool's own error, once it has scratch to clear.
def bisect_die(msg)
  bisect_cleanup
  die(msg, 4)
end

# What one run of a build did.
class BisectRun
  attr_reader :stdout, :status, :signaled, :timed_out
  def initialize(stdout, status, signaled, timed_out)
    @stdout = stdout
    @status = status
    @signaled = signaled
    @timed_out = timed_out
  end

  def same_as(o)
    @stdout == o.stdout && @status == o.status && @signaled == o.signaled && @timed_out == o.timed_out
  end
end

# Run a shell command with stdin closed and stdout captured, under the time
# limit, as tools/diff.rb's run_captured does. Past the limit the command
# is killed with everything it started (an oracle's pipeline, the binary a
# script ran).
def bisect_run(cmd, out_path, cwd)
  pid = Process.spawn(cmd, in: "/dev/null", out: out_path, err: "/dev/null", chdir: cwd, pgroup: true)
  $bisect_running = pid
  timed_out = 0
  done = false
  watchdog = Thread.new do
    slept = 0.0
    while !done && slept < $bisect_timeout
      sleep 0.05
      slept += 0.05
    end
    if !done
      timed_out = 1
      begin
        Process.kill("KILL", -pid)
      rescue StandardError
        nil
      end
    end
  end
  _pid, status = Process.waitpid2(pid)
  $bisect_running = 0
  done = true
  watchdog.join
  out = File.exist?(out_path) ? File.read(out_path) : ""
  sig = status.signaled? && timed_out == 0 ? 1 : 0
  st = status.exitstatus
  st = -1 if st.nil?
  BisectRun.new(out, st, sig, timed_out)
end

# The program under one subset of its decisions: builds it, and says whether
# that build is right.
class BisectProbe
  attr_reader :builds, :last, :relative

  # mode: "self" (against the deny-all build), "expected", "cruby", "cmd"
  def initialize(spinel, src, flags, mode, want, ocmd, tmp)
    @spinel = spinel
    @src = src
    @flags = flags
    @mode = mode
    @want = want
    @ocmd = ocmd
    @tmp = tmp
    @cwd = dir_name(src)
    @bin = File.expand_path(tmp + ".bin")
    @allow = tmp + ".allow"
    @builds = 0
    @last = BisectRun.new("", -1, 0, 0)
    @ref = BisectRun.new("", -1, 0, 0)
    @relative = mode == "self"
  end

  def files
    [@bin, @allow, @tmp + ".out", @tmp + ".c", @tmp + ".log"]
  end

  # From here on a build is wrong when it differs from `run`.
  def relative_to(run)
    @ref = run
    @relative = true
  end

  # What each command of a probe starts with: the registry's two variables
  # as this probe wants them, whatever the tool's own environment holds.
  def scope(restricted)
    s = "unset SPINEL_DECISIONS SPINEL_DECISIONS_LOG; "
    s = s + "export SPINEL_DECISIONS=" + shell_word(@allow) + "; " if restricted
    s
  end

  def compiler
    shell_word(@spinel) + " " + shell_word(@src) + @flags
  end

  # The keys the unrestricted compile takes, in the order it asks them.
  def log_keys
    log = @tmp + ".log"
    sh(scope(false) + "SPINEL_DECISIONS_LOG=" + shell_word(log) + " " + compiler + " -c -o " + shell_word(@tmp + ".c"))
    ok = $sh_status == 0 && File.exist?(log)
    keys = []
    File.read(log).split("\n").each { |k| keys.push(k) if k.length > 0 } if ok
    bisect_die("spinel-bisect: spinel does not compile " + @src) if !ok
    keys
  end

  # BS_GOOD, BS_BAD or BS_SKIP for the program with exactly `keys` allowed.
  def probe(keys)
    File.write(@allow, keys.join("\n") + (keys.length > 0 ? "\n" : ""))
    judge(true)
  end

  # The same for the unrestricted program.
  def probe_unrestricted
    judge(false)
  end

  # Does the binary just built do again what it did? A program that prints
  # the time or its pid differs from every build of itself, and each key in
  # turn would look like the one that changed it. Asked of the two builds
  # every other one is compared with.
  def repeats
    return true if @mode == "cruby" || @mode == "cmd"
    bisect_run("exec " + shell_word(@bin), @tmp + ".out", @cwd).same_as(@last)
  end

  def judge(restricted)
    @builds += 1
    if @mode == "cruby"
      out = sh(scope(restricted) + shell_word(@spinel) + " diff " + shell_word(@src) + " --timeout " + $bisect_timeout.to_s + "; echo \"rc=$?\"")
      rc = trailing_rc(out)
      bisect_die("spinel-bisect: spinel diff could not run (its exit status 4)") if rc == 4
      return oracle_answer(rc, 2)
    end
    if @mode == "cmd" && !@ocmd.include?("{}")
      return oracle_verdict(bisect_run(scope(restricted) + "exec sh -c " + shell_word(@ocmd), @tmp + ".out", "."))
    end
    sh(scope(restricted) + compiler + " -o " + shell_word(@bin))
    return BS_SKIP if $sh_status != 0
    if @mode == "cmd"
      return oracle_verdict(bisect_run(scope(false) + "exec sh -c " + shell_word(@ocmd.gsub("{}", @bin)), @tmp + ".out", "."))
    end
    @last = bisect_run("exec " + shell_word(@bin), @tmp + ".out", @cwd)
    if @relative
      return @last.same_as(@ref) ? BS_GOOD : BS_BAD
    end
    right = @last.stdout == @want && @last.status == 0 && @last.signaled == 0 && @last.timed_out == 0
    right ? BS_GOOD : BS_BAD
  end
end

# An oracle command's exit status as an answer; `skip` is the status that
# means it could not tell.
def oracle_answer(status, skip)
  return BS_GOOD if status == 0
  return BS_SKIP if status == skip
  BS_BAD
end

# An oracle command's run as an answer. One the time limit ended said
# nothing about the build.
def oracle_verdict(run)
  return BS_SKIP if run.timed_out == 1
  oracle_answer(run.status, 125)
end

# The number after the last `rc=` of a captured command.
def trailing_rc(out)
  i = out.rindex("rc=")
  i ? out[i + 3, out.length - i - 3].to_i : 4
end

def plural(n, word)
  n.to_s + " " + word + (n == 1 ? "" : "s")
end

def main
  args = ARGV.dup
  src = nil
  flags = ""
  mode = "self"
  expected = nil
  ocmd = ""
  keep = false
  i = 0
  while i < args.length
    a = args[i]
    if a == "--"
      args[i + 1, args.length - i - 1].each { |f| flags = flags + " " + shell_word(f) }
      break
    elsif a == "--keep-tmp"
      keep = true
    elsif a == "--cruby"
      mode = "cruby"
    elsif a == "--expected"
      i += 1
      die("spinel-bisect: --expected needs a file", 4) if i >= args.length
      expected = args[i]
      mode = "expected"
    elsif a == "--oracle-cmd"
      i += 1
      die("spinel-bisect: --oracle-cmd needs a command", 4) if i >= args.length
      ocmd = args[i]
      mode = "cmd"
    elsif a == "--timeout"
      i += 1
      die("spinel-bisect: --timeout needs a number of seconds", 4) if i >= args.length
      $bisect_timeout = args[i].to_i
      die("spinel-bisect: --timeout needs a positive number of seconds", 4) if $bisect_timeout <= 0
    elsif a == "-h" || a == "--help"
      puts "usage: spinel bisect FILE.rb [--expected FILE | --cruby | --oracle-cmd CMD] [--timeout SEC] [--keep-tmp] [-- COMPILER-FLAGS...]"
      exit(0)
    elsif a.length > 1 && a[0] == "-"
      die("spinel-bisect: unknown option " + a, 4)
    else
      die("spinel-bisect: one program at a time (" + src + " and " + a + ")", 4) if src
      src = a
    end
    i += 1
  end
  die("usage: spinel bisect FILE.rb [options] [-- COMPILER-FLAGS...]", 4) if src.nil?
  die("spinel-bisect: no such file: " + src, 4) if !File.exist?(src)
  want = ""
  if expected
    die("spinel-bisect: no such file: " + expected, 4) if !File.exist?(expected)
    want = File.read(expected)
  end
  # `spinel diff` builds the program itself and takes no compiler flags
  die("spinel-bisect: --cruby takes no compiler flags", 4) if mode == "cruby" && flags.length > 0

  spinel = find_spinel(4)
  # two bisects of one program at a time do not share their scratch
  tmp = tmp_path("bisect", src, "-" + Process.pid.to_s)
  probe = BisectProbe.new(spinel, src, flags, mode, want, ocmd, tmp)
  $bisect_scratch = probe.files if !keep
  Signal.trap("INT") do
    Process.kill("KILL", -$bisect_running) if $bisect_running > 0
    bisect_cleanup
    exit(130)
  end
  search = BisectSearch.new(probe)
  verdict = ""
  code = 0
  found = []
  note = ""
  rest = BS_BAD
  unsteady = "the program does not do the same twice (one build, run again, differs)"

  all = probe.log_keys
  first = probe.probe_unrestricted
  unrestricted = probe.last
  if mode != "self" && first == BS_GOOD
    verdict = "nothing to bisect"
    note = "the unrestricted build is right"
    code = 2
  elsif first == BS_SKIP
    verdict = "inconclusive"
    note = "the unrestricted build could not be judged"
    code = 3
  elsif !probe.repeats
    verdict = "inconclusive"
    note = unsteady
    code = 3
  elsif all.length == 0
    verdict = "no keyed decision changes the answer"
    note = "the compile takes no keyed decision"
    code = 1
  else
    none = probe.probe([])
    denied = probe.last
    if none == BS_SKIP
      verdict = "inconclusive"
      note = "the program does not build with every keyed decision denied"
      code = 3
    elsif !probe.repeats
      verdict = "inconclusive"
      note = unsteady
      code = 3
    elsif mode == "self" ? denied.same_as(unrestricted) : none == BS_BAD
      # wrong either way: the same wrong, or a different one that some
      # decision is still answerable for
      if mode == "expected" && !denied.same_as(unrestricted)
        probe.relative_to(denied)
        note = "The program is wrong with every keyed decision denied as well; these change what it answers."
      else
        verdict = "no keyed decision changes the answer"
        note = mode == "self" ? "the program does the same with every keyed decision denied" : "the program is as wrong with every keyed decision denied"
        code = 1
      end
    elsif mode == "self"
      probe.relative_to(denied)
    end
    if code == 0
      search.know([], BS_GOOD)
      # the compiler given its own log must be the compiler unrestricted
      if search.ask(all) != BS_BAD
        verdict = "inconclusive"
        note = "the program built from its own decision log is not the unrestricted program"
        code = 3
      else
        found = search.run(all)
        if found.length == all.length && search.skips > 0
          verdict = "inconclusive"
          note = "no smaller set of decisions could be tested (" + plural(search.skips, "subset") + " did not build)"
          code = 3
        else
          verdict = "localized"
          rest = search.ask(bs_without(all, found))
        end
      end
    end
  end

  puts "spinel bisect: " + verdict
  puts "  program: " + src
  if mode == "expected"
    puts "  oracle:  stdout is " + expected + ", exit status 0"
  elsif mode == "cruby"
    puts "  oracle:  spinel diff against CRuby"
  elsif mode == "cmd"
    puts "  oracle:  " + ocmd
  else
    puts "  oracle:  the program with every keyed decision denied"
  end
  if verdict == "localized"
    # judged against the build with every decision denied, whatever the
    # oracle: under --expected that build is wrong too, and a build that
    # answers as it does is not right
    rel = probe.relative
    wrong = rel ? "differs" : "is wrong"
    puts "  The program " + wrong + " with " + (found.length == 1 ? "this decision" : "these " + found.length.to_s + " decisions together") +
         " and no other (of " + all.length.to_s + " taken):"
    width = 0
    found.each { |k| width = bs_kind(k).length if bs_kind(k).length > width }
    found.each { |k| puts "    " + bs_kind(k).ljust(width) + "  " + bs_site(k) }
    puts "  " + note if note.length > 0
    it = found.length == 1 ? "it" : "them"
    if rest == BS_GOOD
      same = mode == "self" ? "does what the reference does." : "answers as it does with every keyed decision denied."
      puts "  With only " + it + " denied the program " + (rel ? same : "is right.")
    elsif rest == BS_SKIP
      puts "  With only " + it + " denied the program could not be judged."
    else
      puts "  With only " + it + " denied the program " + (rel ? "still differs" : "is still wrong") + ": other decisions do the same."
    end
  elsif note.length > 0
    puts "  " + note
  end
  puts "  " + plural(probe.builds, "build") + (search.skips > 0 ? ", " + search.skips.to_s + " not judged" : "")
  if keep && verdict == "localized"
    File.write(tmp + ".bad.allow", found.join("\n") + "\n")
    File.write(tmp + ".good.allow", bs_without(all, found).join("\n") + "\n")
    puts "  allow-lists: " + tmp + ".bad.allow (wrong), " + tmp + ".good.allow (all the others)"
  end
  puts "  scratch kept under " + tmp + ".*" if keep
  bisect_cleanup
  found.each { |k| puts "key " + k } if verdict == "localized"
  exit(code)
end

main
