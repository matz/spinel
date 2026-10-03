# spinel-diff: run one program under CRuby and under spinel and compare what
# the two did -- stdout, the uncaught exception, the exit status -- with the
# non-deterministic parts folded first (tools/diff_normalize.rb) and the result
# put under one label (tools/diff_classify.rb). This is the tool a bug report
# starts from: the same input to both runtimes, compared mechanically, so the
# report says what differs rather than what someone noticed.
#
# Usage: spinel diff FILE.rb [options] [-- ARGS...]
#
#   --no-minimize      skip the minimization (the default until it exists)
#   --emit-issue PATH  write the report to PATH instead of stdout
#   --timeout SEC      per-run time limit (default 30)
#   --ruby PATH        the reference ruby (default: `ruby` on PATH)
#   --keep-tmp         leave the scratch files (the C, the binary, the runs)
#
# Exit status (a contract, CI reads it):
#   0  no difference
#   1  a difference in output, exception, exit status, a timeout, or a run
#      that disagreed with itself
#   2  spinel could not compile the program (or cc could not build the C)
#   3  the spinel binary ended on a signal
#   4  the tool's own error (no ruby, no file, no cc)
#
# The program runs twice, under two runtimes, with the arguments given: do not
# point this at code you would not run.
#
# Written in the spinel subset and compiled by spinel (see tools/README.md).

require_relative "tool_common"
require_relative "diff_normalize"
require_relative "diff_classify"

$diff_timeout = 30

# The name of a terminating signal, for the report.
def signal_name(n)
  names = { 1 => "SIGHUP", 2 => "SIGINT", 3 => "SIGQUIT", 4 => "SIGILL", 6 => "SIGABRT",
            7 => "SIGBUS", 8 => "SIGFPE", 9 => "SIGKILL", 11 => "SIGSEGV", 13 => "SIGPIPE",
            14 => "SIGALRM", 15 => "SIGTERM" }
  n2 = names[n]
  n2 ? n2 : "signal " + n.to_s
end

# Run `argv` (a command and its arguments) with stdin closed and stdout /
# stderr captured to files, under the time limit. Answers a DiffRun. The
# command goes through `sh -c 'exec ...'`, so the pid the watchdog kills is
# the program's own.
def run_captured(argv, out_path, err_path, cwd)
  cmd = "exec " + argv.map { |a| shell_word(a) }.join(" ")
  pid = Process.spawn(cmd, in: "/dev/null", out: out_path, err: err_path, chdir: cwd)
  timed_out = 0
  done = false
  watchdog = Thread.new do
    slept = 0.0
    while !done && slept < $diff_timeout
      sleep 0.05
      slept += 0.05
    end
    if !done
      timed_out = 1
      begin
        Process.kill("KILL", pid)
      rescue StandardError
        nil
      end
    end
  end
  _pid, status = Process.waitpid2(pid)
  done = true
  watchdog.join
  out = File.exist?(out_path) ? File.read(out_path) : ""
  err = File.exist?(err_path) ? File.read(err_path) : ""
  sig = ""
  sig = signal_name(status.termsig) if status.signaled? && timed_out == 0
  st = status.exitstatus
  st = -1 if st.nil?
  DiffRun.new(out, err, st, sig, timed_out)
end

# `ruby -v` says 3.4 or newer: the spellings spinel writes.
def ruby_pre34(ruby)
  v = sh(ruby + " -e 'print RUBY_VERSION'").strip
  return false if v.length == 0
  parts = v.split(".")
  major = parts[0].to_i
  minor = parts.length > 1 ? parts[1].to_i : 0
  major < 3 || (major == 3 && minor < 4)
end

# A unified diff of two texts, through diff(1), for the report.
def unified(a, b, ta, tb, tmpa, tmpb)
  File.write(tmpa, a)
  File.write(tmpb, b)
  d = sh("diff -u --label '" + ta + "' --label '" + tb + "' " + tmpa + " " + tmpb)
  d
end

def describe_run(r)
  s = "exit " + r.status.to_s
  s = s + " (" + r.signal + ")" if r.signal.length > 0
  s = s + " (timed out after " + $diff_timeout.to_s + "s)" if r.timed_out == 1
  s
end

def main
  args = ARGV.dup
  src = nil
  prog_args = []
  emit_issue = nil
  ruby = "ruby"
  keep = false
  i = 0
  while i < args.length
    a = args[i]
    if a == "--"
      prog_args = args[i + 1, args.length - i - 1]
      break
    elsif a == "--no-minimize"
      nil   # the minimization does not exist yet; accepted for the interface
    elsif a == "--keep-tmp"
      keep = true
    elsif a == "--timeout"
      i += 1
      die("spinel-diff: --timeout needs a number of seconds", 4) if i >= args.length
      $diff_timeout = args[i].to_i
      die("spinel-diff: --timeout needs a positive number of seconds", 4) if $diff_timeout <= 0
    elsif a == "--ruby"
      i += 1
      die("spinel-diff: --ruby needs a path", 4) if i >= args.length
      ruby = args[i]
    elsif a == "--emit-issue"
      i += 1
      die("spinel-diff: --emit-issue needs a path", 4) if i >= args.length
      emit_issue = args[i]
    elsif a == "-h" || a == "--help"
      puts "usage: spinel diff FILE.rb [--no-minimize] [--emit-issue PATH] [--timeout SEC] [--ruby PATH] [--keep-tmp] [-- ARGS...]"
      exit(0)
    elsif a.length > 1 && a[0] == "-"
      die("spinel-diff: unknown option " + a, 4)
    else
      die("spinel-diff: one program at a time (" + src + " and " + a + ")", 4) if src
      src = a
    end
    i += 1
  end
  die("usage: spinel diff FILE.rb [options] [-- ARGS...]", 4) if src.nil?
  die("spinel-diff: no such file: " + src, 4) if !File.exist?(src)
  die("spinel-diff: cannot find " + ruby + " (set --ruby PATH)", 4) if !have_cmd(ruby)
  spinel = find_spinel
  cwd = dir_name(src)
  base = base_name(src)
  cwd = "." if cwd.length == 0
  tmp = tmp_path("diff", src, "")
  bin = tmp + ".bin"
  c_out = tmp + ".c"
  files = [bin, c_out]
  pre34 = ruby_pre34(ruby)
  dirs = [File.expand_path(cwd), tmp_path("diff", src, "")]

  # the reference run
  ref = run_captured([ruby, base] + prog_args, tmp + ".ref.out", tmp + ".ref.err", cwd)
  files.push(tmp + ".ref.out"); files.push(tmp + ".ref.err")

  # the spinel build: no C is the compiler's refusal, a C that does not build
  # is the link error; the compiler says which on stderr
  build = sh(spinel + " " + src + " -o " + bin)
  compile_ok = $sh_status == 0
  link_ok = compile_ok
  if !compile_ok
    if build.include?("C compilation failed")
      compile_ok = true
      link_ok = false
    end
  end
  got = DiffRun.new("", "", -1, "", 0)
  got2 = nil
  if compile_ok && link_ok
    got = run_captured([bin] + prog_args, tmp + ".got.out", tmp + ".got.err", cwd)
    files.push(tmp + ".got.out"); files.push(tmp + ".got.err")
  end

  ref_n = DiffRun.new(dn_stdout(ref.stdout, dirs, pre34), dn_stderr(ref.stderr, dirs, pre34), ref.status, ref.signal, ref.timed_out)
  got_n = DiffRun.new(dn_stdout(got.stdout, dirs, pre34), dn_stderr(got.stderr, dirs, pre34), got.status, got.signal, got.timed_out)
  label = dc_classify(compile_ok, link_ok, ref_n, got_n, nil)
  # a difference has to be the program's, not the run's: the spinel binary
  # runs once more and must agree with itself
  if label == "exception-diff" || label == "output-diff"
    g2 = run_captured([bin] + prog_args, tmp + ".got2.out", tmp + ".got2.err", cwd)
    files.push(tmp + ".got2.out"); files.push(tmp + ".got2.err")
    got2 = DiffRun.new(dn_stdout(g2.stdout, dirs, pre34), dn_stderr(g2.stderr, dirs, pre34), g2.status, g2.signal, g2.timed_out)
    label = dc_classify(compile_ok, link_ok, ref_n, got_n, got2)
  end

  # the report
  rep = "spinel diff: " + label + "\n"
  rep = rep + "  program: " + src
  rep = rep + " " + prog_args.map { |a| shell_word(a) }.join(" ") if prog_args.length > 0
  rep = rep + "\n"
  rep = rep + "  ruby:    " + describe_run(ref) + "\n"
  if label == "compile-error" || label == "link-error"
    rep = rep + "  spinel:  " + (label == "compile-error" ? "refused the program" : "the C did not build") + "\n"
    rep = rep + "\n" + build
  else
    rep = rep + "  spinel:  " + describe_run(got) + "\n"
    if label == "nondeterministic"
      rep = rep + "  the spinel binary disagreed with its own second run; a value that changes per run (rand, a pid, a time, thread order) is in the output\n"
    end
    if ref_n.stderr != got_n.stderr
      rep = rep + "  exception (ruby):   " + (ref_n.stderr.length > 0 ? ref_n.stderr : "(none)") + "\n"
      rep = rep + "  exception (spinel): " + (got_n.stderr.length > 0 ? got_n.stderr : "(none)") + "\n"
    end
    if ref_n.stdout != got_n.stdout
      rep = rep + "\n" + unified(ref_n.stdout, got_n.stdout, "stdout (ruby)", "stdout (spinel)", tmp + ".a", tmp + ".b")
      files.push(tmp + ".a"); files.push(tmp + ".b")
    end
  end
  if emit_issue
    File.write(emit_issue, rep)
    puts "spinel diff: " + label + " (report written to " + emit_issue + ")"
  else
    print rep
  end
  if !keep
    files.each do |f|
      begin
        File.delete(f) if File.exist?(f)
      rescue StandardError
        nil
      end
    end
  else
    puts "  scratch kept under " + tmp + ".*"
  end
  exit(dc_exit_code(label))
end

main
