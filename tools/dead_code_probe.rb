# Dead code probe: code that never runs added to a program, its answer compared.
#
#   ruby tools/dead_code_probe.rb [--kind K,..] [--alien A] [--together | --each]
#                                 [--sample N] [--seed S] [--opt LEVEL] [--jobs J]
#                                 [--out DIR] [--timeout SEC] [--builds N] [--keep]
#                                 [FILE..]
#
# Spinel types a slot from every write it can see, and decides from the
# types which path a read, a call or a store takes. A statement that never
# runs is still a write it sees: `x = :dead if ::ARGV.length == 9123` in
# front of `x = 1` boxes `x`, and every use of `x` leaves the typed path
# for the boxed one. The program does what it did, so it has to print what
# it printed. The probe asks whether it does. It takes each FILE (default
# the test/*.rb the suite runs without a flag, or `--sample N` of them,
# picked by `--seed`) that has a .expected beside it, puts such statements
# into it (tools/dead_code_edit.rb: eleven kinds, one per decision the edit
# takes from the compiler), builds the edited program, runs it as the suite
# runs the test (its .args and .stdin, from the root of the tree) and
# compares stdout and stderr with the .expected the unedited test is held
# to. No other oracle is needed, and every test of the corpus is a test of
# the boxed path it does not name.
#
# A pass is one build of a program with a set of edits in it. By default
# there is a pass per kind, with every site of that kind edited; `--together`
# makes one pass of every kind at once (the fewest builds; it boxes both
# sides of every boundary, so it sees less), `--each` a pass per site (the
# most builds; it sees an edit that only fails beside typed neighbours).
# `--kind` names the kinds to use, `--alien` the value the edits write
# (sym, str, int, float, nil, ary, obj; default sym, `:dead`). A build is
# most of the cost, so the programs are built at `-Og` unless `--opt` names
# another level: the question is what spinel decides, not what the C
# compiler makes of it.
#
# A pass that does not print the answer is cut down to the edits that carry
# the difference: the edits on the line a failed build names are tried
# first, then the set is halved for as long as a part still fails the same
# way (ddmin). Those edits are a finding, and the pass is tried again
# without them, since one refusal would hide every other edit of the pass.
# A program is given `--builds` C compiles for this beyond its passes
# (default 40); a finding the budget stopped short says so. C that does not
# build is cut down asking the C compiler for its checks only.
#
# What counts follows the compiler's contract, in the labels and tiers of
# the generated-case probes (tools/probe_common.rb). `wrong`: another
# answer (output-diff), a crash, a timeout, C that does not build
# (link-error: the compiler should have refused), a compiler that dies or
# fails without a word of its own (compiler-failure). `refused`: the
# compiler said, in its own name, what it will not compile
# (compile-error): a construct it does not support, a call that cannot
# exist, a value it will not keep in a slot of another type. CONTRIBUTING.md
# asks for that instead of a wrong answer, so it is listed and not counted.
# The probe does not read docs/limitations.md: a wrong answer the limits
# describe is still listed.
#
# Before a difference is a finding, three things are checked. The unedited
# program, copied to the scratch directory and built there, has to print
# its .expected: a test that reads a file beside itself, or fails on this
# machine, is left out. The edited program has to print under this ruby
# what the unedited one prints under it (both run with
# --enable-frozen-string-literal, as the corpus's reference is): an edit
# that changed the program is the tool's mistake and is listed apart. And
# an answer that differs is asked for a second time. One kind is a control:
# `nop` puts `nil` under the guard and takes no decision from the compiler,
# so what it finds is not about types, and a difference another kind shows
# on the same line in the same words is counted once, as the control's.
#
# Output, under DIR (default build/dead-code-probe): summary.txt (the
# findings by tier, label and kind, and in families: for a build that
# failed the words of the failure, for a run that ended in an exception
# CRuby does not raise the exception, else the kind of edit and the type
# the slot had before it, from the unedited program's --emit-types dump)
# and, for each finding, findings/<n>-<program>/ with a.rb (the test),
# b.rb (the test with the edits of the finding and no others, a program
# that answers differently under CRuby and spinel and can be handed to
# `spinel diff` and spinel-reduce as it is) and finding.txt (the edits,
# the slot's type, the lines that changed and what the build or the run
# said).
#
# A probe to run by hand, like order_probe: not a gate, and not one of the
# tools make builds.
#
# Exit status: 0 no wrong answer, 1 at least one, 4 the tool's own error.

require "fileutils"
require "json"
require "rbconfig"
require "tmpdir"
require_relative "dead_code_edit"
require_relative "probe_common"

module DeadCodeProbe
  NAME = "dead_code_probe".freeze
  # most severe first; all but the last are the `wrong` tier
  LABELS = %w[output-diff crash timeout link-error compiler-failure compile-error].freeze
  WRONG = LABELS.first(5).freeze
  # what changes the text spinel parses or the C it writes (src/spinel_parse.c)
  PINNED = %w[SPINEL_NO_BUILTINS SPINEL_DEBUG SPINEL_LINE_MAP SPINEL_REQUIRE_GATE SPINEL_EMIT_TYPES
              SPINEL_WARN_WIDEN].to_h { |v| [v, nil] }.freeze
  BUILD_TIMEOUT = 600

  # What one build of a program came to. label is "same" (it printed the
  # answer) or one of LABELS; detail the first line of what the build said,
  # or the first line of the answer that differs; got the [stdout, stderr]
  # of a run; line the line of the program a failed build names.
  Outcome = Struct.new(:label, :detail, :got, :line)

  # A set of edits a program does not print its answer with. pass names the
  # pass that showed it; minimal is false when the build budget ended the
  # cutting down; type is the type the first edit's slot had in the unedited
  # program; text is b.rb.
  Finding = Struct.new(:path, :label, :sites, :detail, :got, :pass, :minimal, :type, :text, :dir) do
    def kinds = sites.map(&:kind).uniq
    def tier = WRONG.include?(label) ? "wrong" : "refused"
  end

  # One program under the probe: its edits, its answer, the outcome of
  # every set of edits built so far and the C compiles spent cutting down.
  Program = Struct.new(:path, :source, :sites, :want, :args, :stdin, :dir, :seen, :spent, :files)

  # The program cannot be probed; the message says why.
  class LeftOut < StandardError; end

  class Probe
    attr_reader :findings

    def initialize(spinel, root, work, timeout:, builds:, kinds:, alien:, mode:, opt:, keep:)
      @spinel = spinel
      @opt = opt
      @root = root
      @work = work
      @timeout = timeout
      @builds = builds
      @kinds = kinds
      @alien = alien
      @mode = mode
      @keep = keep
      @findings = []
      @skips = Hash.new { |h, k| h[k] = [] }
      @changed = [] # [path, sites]: edits this ruby says changed the program
      @counts = Hash.new(0)
      @site_counts = Hash.new(0)
      @lock = Mutex.new
      @stopped = false
      @halt = -> { @stopped }
    end

    # Stops the probe: every build and run in flight is killed, and it and
    # every later one raise ProbeCommon::Stopped.
    def stop
      @stopped = true
    end

    # Waits for `threads` that build. An interrupt, which reaches the main
    # thread only, stops them first (see OrderProbe::Probe#finish).
    def finish(threads)
      threads.each(&:join)
    ensure
      if threads.any?(&:alive?)
        stop
        threads.each(&:join)
      end
    end

    def skip(path, why)
      @lock.synchronize { @skips[why] << path }
    end

    def relative(path)
      File.expand_path(path).delete_prefix("#{@root}/")
    end

    # One program: its passes, each cut down to findings.
    def check(path, id)
      source = File.binread(path)
      return skip(path, "no .expected beside it") unless File.file?("#{path}.expected")
      begin
        sites = DeadCodeEdit.sites(source, alien: @alien).select { |s| @kinds.include?(s.kind) }
      rescue DeadCodeEdit::ParseError
        return skip(path, "ruby #{RUBY_VERSION} does not parse it")
      end
      return skip(path, "nothing to edit") if sites.empty?
      want = [lines("#{path}.expected"), File.file?("#{path}.err.expected") ? lines("#{path}.err.expected") : ""]
      args = File.file?("#{path}.args") ? File.read("#{path}.args").split : []
      stdin = File.file?("#{path}.stdin") ? "#{path}.stdin" : File::NULL
      prog = Program.new(path, source, sites, want, args, stdin, File.join(@work, id.to_s), {}, 0, 0)
      found = []
      begin
        passes(sites).each { |label, set| pass(prog, label, set, found) }
      rescue LeftOut => e
        return skip(path, e.message)
      end
      found = found.uniq { |f| f.sites.map(&:id) }.select { |f| believed?(prog, f) }
      # a difference the control shows on the same line is the control's
      shown = ->(f) { [f.sites.map(&:line), f.label, ProbeCommon.error_kind(f.detail)] }
      nops = found.select { |f| f.kinds == ["nop"] }.map(&shown)
      found.reject! { |f| f.kinds != ["nop"] && nops.include?(shown.(f)) }
      types = found.empty? ? {} : slot_types(prog)
      found.each { |f| f.type = types[f.sites.first.id] }
      @lock.synchronize do
        @counts[:probed] += 1
        @counts[:builds] += prog.files
        sites.each { |s| @site_counts[s.kind] += 1 }
        @findings.concat(found)
      end
    ensure
      FileUtils.rm_rf(prog.dir) if prog && !@keep
    end

    # The passes of a program: [label, sites] each.
    def passes(sites)
      case @mode
      when "together" then [["together", sites]]
      when "each" then sites.map { |s| ["#{s.kind} #{s.name}", [s]] }
      else sites.group_by(&:kind).to_a
      end
    end

    # A pass, and again without the edits of each finding it gives, until
    # what is left prints the answer.
    def pass(prog, label, sites, found)
      live = sites
      until live.empty?
        first = outcome(prog, live)
        break if first.label == "same"
        core, minimal = cut_down(prog, live, first.label)
        o = outcome(prog, core)
        found << Finding.new(prog.path, o.label, core, o.detail, o.got, label, minimal, nil,
                             DeadCodeEdit.apply(prog.source, core))
        break unless minimal
        live -= core
      end
    end

    # The fewest of `sites` that still give `label` (ddmin: a part that
    # fails alone, else the whole without a part, in finer parts each
    # round), and whether the build budget let the cutting down end.
    def cut_down(prog, sites, label)
      quick = label == "link-error" && outcome(prog, sites, check_only: true).label == label
      fails = lambda do |set|
        fresh = !prog.seen.key?([set.map(&:id), quick])
        raise StopIteration if fresh && prog.spent >= @builds
        o = outcome(prog, set, check_only: quick)
        # a refusal costs no C compile
        prog.spent += 1 if fresh && o.label != "compile-error"
        o.label == label
      end
      # the edits on the line a failed build names are tried first (a C
      # error names one through spinel's #line map)
      named = sites.select { |x| x.line == outcome(prog, sites).line }
      sites = named if !named.empty? && named.size < sites.size && fails.(named)
      n = 2
      while sites.size > 1
        parts = sites.each_slice((sites.size / n.to_f).ceil).to_a
        if (part = parts.find { |p| fails.(p) })
          sites = part
          n = 2
        elsif n > 2 && (part = parts.find { |p| fails.(sites - p) })
          sites -= part
          n -= 1
        elsif n < sites.size
          n = [n * 2, sites.size].min
        else
          break
        end
      end
      [sites, true]
    rescue StopIteration
      [sites, false]
    end

    # What the program comes to with `sites` edited in: built and run once
    # for each set. check_only stops the build at the C compiler's checks,
    # and answers "same" for C that passes them.
    def outcome(prog, sites, check_only: false)
      prog.seen[[sites.map(&:id), check_only]] ||= begin
        o = build_and_run(prog, sites, check_only)
        # an edit is at fault only where the unedited copy builds and
        # prints the answer
        reference(prog) unless sites.empty? || o.label == "same"
        o
      end
    end

    # The unedited program, from the scratch directory. One that does not
    # print its .expected there is left out.
    def reference(prog)
      o = outcome(prog, [])
      return o if o.label == "same"
      raise LeftOut, "its copy does not print the .expected (#{o.label})"
    end

    def build_and_run(prog, sites, check_only)
      dir = File.join(prog.dir, (prog.files += 1).to_s)
      FileUtils.mkdir_p(dir)
      # each copy keeps the test's own file name: __FILE__ prints the same
      src = File.join(dir, File.basename(prog.path))
      File.binwrite(src, DeadCodeEdit.apply(prog.source, sites))
      bin = File.join(dir, "bin")
      log = File.join(dir, "build")
      syntax_only = check_only ? ["--cc=cc -fsyntax-only"] : []
      argv = [PINNED, @spinel, *syntax_only, "-O", @opt, src, "-o", bin]
      status, timed_out = ProbeCommon.run_timed(argv, BUILD_TIMEOUT, log, log, @halt)
      return build_failure(File.binread(log), status, timed_out) unless !timed_out && status.success?
      return Outcome.new("same", "") if check_only
      o = run(prog, dir, bin)
      # an answer that is not the expected one is asked for a second time
      o.label == "same" ? o : run(prog, dir, bin)
    ensure
      FileUtils.rm_f(bin) if bin && !@keep
    end

    # One run of a binary, as the suite runs a test: its stdout and stderr
    # against the expected ones, whatever its exit status.
    def run(prog, dir, bin)
      out = File.join(dir, "out")
      err = File.join(dir, "err")
      status, timed_out = ProbeCommon.run_timed([bin, *prog.args], @timeout, out, err, @halt, input: prog.stdin)
      got = [lines(out), lines(err)]
      return Outcome.new("timeout", "no answer after #{@timeout}s", got) if timed_out
      return Outcome.new("crash", "SIG#{Signal.signame(status.termsig)}", got) if status.signaled?
      return Outcome.new("same", "", got) if got == prog.want
      Outcome.new("output-diff", first_difference(prog.want, got), got)
    end

    # A build that did not end in a binary. C that does not build comes
    # first, since its diagnostics can quote generated C. Then whatever the
    # compiler says in its own name is a refusal: a construct it does not
    # compile, a call that cannot exist (docs/limitations.md), a value it
    # will not put in a slot of another type.
    def build_failure(said, status, timed_out)
      said = said.scrub
      mine = said.lines.find { |l| l.start_with?("spinel: ") && !l.start_with?("spinel: warning") }
      label = if timed_out || status.signaled? then "compiler-failure"
              elsif said.include?("C compilation failed") then "link-error"
              else mine ? "compile-error" : "compiler-failure"
              end
      first = if timed_out then "spinel ran past #{BUILD_TIMEOUT}s"
              elsif status.signaled? then "spinel died of SIG#{Signal.signame(status.termsig)}"
              elsif label == "link-error" then said.lines.find { |l| l.include?("error:") }
              else mine
              end
      first = (first || said.lines.last.to_s).strip.sub(/\Aspinel: /, "")
      Outcome.new(label, first.sub(ProbeCommon::LOCATION, ""), nil, first[/\A\S+\.rb:(\d+):/, 1]&.to_i)
    end

    # A file as the suite compares it: without the carriage returns.
    def lines(path)
      File.binread(path).gsub("\r\n", "\n")
    end

    def first_difference(want, got)
      stream = want[0] == got[0] ? 1 : 0
      w = want[stream].lines
      g = got[stream].lines
      i = (0...[w.size, g.size].max).find { |k| w[k] != g[k] }
      show = ->(l) { l ? l.chomp.scrub[0, 70].inspect : "(nothing)" }
      "#{stream == 1 ? "stderr " : ""}line #{i + 1}: #{show.(w[i])} became #{show.(g[i])}"
    end

    # Whether this ruby prints the same from the edited program as from the
    # test: an edit that changed the program is no finding.
    def believed?(prog, finding)
      a = ruby_run(prog, [])
      b = ruby_run(prog, finding.sites)
      return true if a == b
      @lock.synchronize { @changed << [prog.path, finding.sites] }
      false
    end

    # [stdout, exit status] of the program under the ruby running the probe.
    def ruby_run(prog, sites)
      prog.seen[[:ruby, sites.map(&:id)]] ||= begin
        dir = File.join(prog.dir, (prog.files += 1).to_s)
        FileUtils.mkdir_p(dir)
        src = File.join(dir, File.basename(prog.path))
        File.binwrite(src, DeadCodeEdit.apply(prog.source, sites))
        out = File.join(dir, "out")
        argv = [RbConfig.ruby, "--enable-frozen-string-literal", src, *prog.args]
        status, timed_out = ProbeCommon.run_timed(argv, @timeout * 2, out, File::NULL, @halt, input: prog.stdin)
        [lines(out), timed_out ? "timeout" : status.to_i]
      end
    end

    # { site id => the RBS type its slot has in the unedited program }, read
    # from the type dump. The compiler puts a line ahead of the program for
    # each builtin file it uses, so every line is off by one constant: the
    # one under which the most slots have a record.
    def slot_types(prog)
      dir = File.join(prog.dir, "types")
      FileUtils.mkdir_p(dir)
      src = File.join(dir, File.basename(prog.path))
      File.binwrite(src, prog.source)
      json = File.join(dir, "types.json")
      ProbeCommon.run_timed([PINNED, @spinel, src, "--emit-types", "-o", json], BUILD_TIMEOUT,
                            File::NULL, File::NULL, @halt)
      return {} unless File.file?(json)
      records = {}
      JSON.parse(File.binread(json).force_encoding("UTF-8").scrub)["types"].each do |r|
        next unless r["file"] == src
        records[[r["line"], r["col"], r["kind"]]] ||= r["kind"] == "DefNode" ? r["signature"] : r["rbs"]
      end
      slots = prog.sites.select(&:at)
      shift = (0..64).max_by { |s| [slots.count { |x| records.key?([x.at[0] + s, x.at[1], x.at[2]]) }, -s] }
      slots.to_h { |x| [x.id, records[[x.at[0] + shift, x.at[1], x.at[2]]]] }
    rescue JSON::ParserError
      {}
    end

    def write_findings(out)
      @findings.each_with_index do |f, i|
        f.dir = File.join(out, "findings", format("%03d-%s", i + 1, File.basename(f.path, ".rb")))
        FileUtils.mkdir_p(f.dir)
        File.binwrite(File.join(f.dir, "a.rb"), File.binread(f.path))
        File.binwrite(File.join(f.dir, "b.rb"), f.text)
        File.write(File.join(f.dir, "finding.txt"), finding_text(f))
      end
    end

    def finding_text(f)
      cut = f.minimal ? "" : " (the build budget ended the cutting down)"
      s = [relative(f.path), "#{f.label} (#{f.tier}), pass: #{f.pass}#{cut}", f.detail, "", "edits:"]
      f.sites.each_with_index do |x, i|
        s << "  line #{x.line}  #{x.kind} #{x.name}#{i.zero? && f.type ? "  (#{f.type})" : ""}"
      end
      edited = f.text.lines
      File.binread(f.path).lines.each_with_index do |l, i|
        next if l == edited[i]
        s << "" << "line #{i + 1}:" << "  a.rb  #{l.chomp.scrub}" << "  b.rb  #{edited[i].to_s.chomp.scrub}"
      end
      if f.got
        s << "" << "stdout (b.rb):" << f.got[0].scrub
        s << "stderr (b.rb):" << f.got[1].scrub unless f.got[1].empty?
      end
      s.join("\n") + "\n"
    end

    # A finding's family: for a build that failed the words of the
    # failure, for a run that ended in an exception CRuby does not raise
    # the exception, and else the kind of edit and what the slot held.
    def family(f)
      return ProbeCommon.error_kind(f.detail) unless WRONG.first(3).include?(f.label)
      raised = f.got && f.got[1].lines.find { |l| l.match?(/\(\w+(?:::\w+)*\)$/) }
      return raised.strip.sub(ProbeCommon::LOCATION, "").gsub(/\d+/, "N")[0, 100] if raised
      "#{f.kinds.join("+")}#{f.type ? " of #{f.type.gsub(/\d+/, "N")}" : ""}"
    end

    def summary(given, out)
      s = ["programs: #{given} given, #{@counts[:probed]} probed, #{@counts[:builds]} builds and runs"]
      counts = DeadCodeEdit::KINDS.select { |k| @site_counts[k].positive? }
      s << "sites: " + counts.map { |k| "#{k} #{@site_counts[k]}" }.join(", ")
      unless @skips.empty?
        s << "left out:"
        @skips.sort_by { |why, ps| [-ps.size, why] }.each { |why, ps| s << format("  %5d  %s", ps.size, why) }
      end
      unless @changed.empty?
        s << "edits that changed the program under #{RUBY_ENGINE} #{RUBY_VERSION} " \
             "(the tool's mistake; not findings):"
        @changed.each do |path, sites|
          s << "  #{relative(path)}  #{sites.map { |x| "#{x.kind} #{x.name} (line #{x.line})" }.join(", ")}"
        end
      end
      s << ""
      wrong = @findings.count { |f| f.tier == "wrong" }
      s << "findings: #{@findings.size} in #{@findings.map(&:path).uniq.size} programs: #{wrong} wrong, " \
           "#{@findings.size - wrong} refused (not counted)"
      LABELS.each do |l|
        fs = @findings.select { |f| f.label == l }
        next if fs.empty?
        kinds = fs.flat_map(&:kinds).tally.sort_by { |k, n| [-n, k] }.map { |k, n| "#{k} #{n}" }.join(", ")
        s << format("  %-17s %4d  %s", l, fs.size, kinds)
      end
      LABELS.each do |l|
        fs = @findings.select { |f| f.label == l }
        next if fs.empty?
        s << "" << "#{l}:"
        fs.group_by { |f| family(f) }.sort_by { |name, g| [-g.size, name] }.each do |name, g|
          s << "  #{name}  (#{g.size})"
          g.each do |f|
            where = f.sites.map { |x| "#{x.kind} #{x.name} line #{x.line}" }.join(", ")
            s << "    #{f.dir.delete_prefix("#{out}/")}  #{where}#{f.minimal ? "" : "  (not cut down)"}"
            s << "      #{f.detail}" if WRONG.first(3).include?(f.label)
          end
        end
      end
      s.join("\n") + "\n"
    end
  end

  USAGE = "usage: ruby tools/#{NAME}.rb [--kind K,..] [--alien A] [--together | --each] [--sample N] " \
          "[--seed S] [--opt LEVEL] [--jobs J] [--out DIR] [--timeout SEC] [--builds N] [--keep] [FILE..]".freeze

  module_function

  # The command line. Answers the exit status.
  def main(argv)
    root = File.expand_path("..", __dir__)
    kinds = DeadCodeEdit::KINDS
    alien = "sym"
    mode = "kind"
    jobs = 4
    timeout = 10
    builds = 40
    sample = nil
    seed = 1
    opt = "g"
    keep = false
    out = File.join(root, "build", "dead-code-probe")
    files = []
    args = argv.dup
    begin
      until args.empty?
        case (arg = args.shift)
        when "--kind"
          kinds = args.shift.to_s.split(",")
          raise ArgumentError if kinds.empty? || !(kinds - DeadCodeEdit::KINDS).empty?
        when "--alien" then alien = (DeadCodeEdit::ALIENS.keys & [args.shift]).first || raise(ArgumentError)
        when "--together", "--each" then mode = arg.delete_prefix("--")
        when "--sample" then sample = Integer(args.shift)
        when "--seed" then seed = Integer(args.shift)
        when "--opt" then opt = args.shift.to_s[/\A[0-3sg]\z/] || raise(ArgumentError)
        when "--jobs" then jobs = Integer(args.shift)
        when "--timeout" then timeout = Integer(args.shift)
        when "--builds" then builds = Integer(args.shift)
        when "--out" then out = File.expand_path(args.shift || raise(ArgumentError))
        when "--keep" then keep = true
        when /\A--/ then raise ArgumentError
        else files << File.expand_path(arg)
        end
      end
      raise ArgumentError unless [jobs, timeout, sample || 1].all?(&:positive?) && !builds.negative?
    rescue ArgumentError, TypeError => e
      warn "#{NAME}: #{e.message}" unless e.message == "ArgumentError"
      warn USAGE
      return 4
    end
    spinel = File.expand_path(ENV["SPINEL"] || File.join(root, "spinel"))
    unless File.executable?(spinel)
      warn "#{NAME}: no spinel at #{spinel} (build it, or set SPINEL)"
      return 4
    end
    # the suite's own tests: the promote_ ones mean what they print only
    # under --int-overflow=promote
    if files.empty?
      files = Dir.glob(File.join(root, "test/*.rb")).reject { |f| File.basename(f).start_with?("promote_") }
    end
    missing = files.reject { |f| File.file?(f) }
    unless missing.empty?
      warn "#{NAME}: no such file: #{missing.first}"
      return 4
    end
    # a sample is the same programs for the same seed, whatever order they came in
    files = files.sort.shuffle(random: Random.new(seed)).first(sample).sort if sample
    # A directory the tool wrote holds its summary, or at least its lock;
    # any other files are someone else's.
    if File.directory?(out) && !Dir.empty?(out) && %w[summary.txt .lock].none? { |f| File.exist?(File.join(out, f)) }
      warn "#{NAME}: #{out} holds files the tool did not write; give --out an empty or new directory"
      return 4
    end
    FileUtils.mkdir_p(out)
    # one run at a time in a directory, held to the end of the run
    dir_lock = File.open(File.join(out, ".lock"), File::RDWR | File::CREAT)
    unless dir_lock.flock(File::LOCK_EX | File::LOCK_NB)
      warn "#{NAME}: another run is using #{out}; give --out another directory"
      return 4
    end

    work = nil
    Thread.report_on_exception = false
    begin
      %w[findings work summary.txt].each { |p| FileUtils.rm_rf(File.join(out, p)) }
      File.write(File.join(out, "summary.txt"), "run in progress\n")
      work = keep ? File.join(out, "work") : Dir.mktmpdir("dead-code-probe")
      FileUtils.mkdir_p(work)
      # a test reads the files it names from the root of the tree
      Dir.chdir(root)
      probe = Probe.new(spinel, root, work, timeout: timeout, builds: builds, kinds: kinds, alien: alien,
                                            mode: mode, opt: opt, keep: keep)
      queue = Queue.new
      files.each_with_index { |f, i| queue << [f, i] }
      done = 0
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      progress = Mutex.new
      workers = Array.new(jobs) do
        Thread.new do
          while (f, i = (queue.pop(true) rescue nil))
            begin
              probe.check(f, i)
            rescue ProbeCommon::Stopped
              break
            rescue StandardError => e
              # one program the tool cannot read is one program left out
              probe.skip(f, "tool error: #{e.class}: #{e.message.lines.first.to_s.strip}")
            end
            progress.synchronize do
              done += 1
              $stderr.print "\r#{done}/#{files.size} programs, #{probe.findings.size} findings"
            end
          end
        end
      end
      probe.finish(workers)
      $stderr.puts
      took = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
      probe.findings.sort_by! { |f| [LABELS.index(f.label), probe.relative(f.path), f.sites.first.id] }
      probe.write_findings(out)
      version = IO.popen([spinel, "--version"], err: File::NULL, &:read).strip
      what = "kinds: #{kinds.join(" ")} (alien #{alien}, #{mode == "kind" ? "a pass per kind" : mode}, -O#{opt})"
      what << "\nsample: #{sample} programs, seed #{seed}" if sample
      report = "spinel: #{version}\nruby: #{RUBY_DESCRIPTION}\n#{what}\n" +
               probe.summary(files.size, out) + format("\nwall time: %ds (jobs %d)\n", took, jobs)
      File.write(File.join(out, "summary.txt"), report)
      puts report
      puts "findings under #{out}" unless probe.findings.empty?
      probe.findings.any? { |f| f.tier == "wrong" } ? 1 : 0
    rescue StandardError => e
      warn "#{NAME}: #{e.message}"
      4
    ensure
      FileUtils.rm_rf(work) if work && !keep
      dir_lock.close
    end
  end
end

exit DeadCodeProbe.main(ARGV) if $PROGRAM_NAME == __FILE__
