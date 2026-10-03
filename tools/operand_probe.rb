# Operand order probe: every operand of every call logged, CRuby against spinel.
#
#   ruby tools/operand_probe.rb [--int-overflow promote] [--jobs J] [--out DIR]
#                               [--timeout SEC] [--keep] [FILE..]
#
# Ruby runs a call's receiver, then its arguments left to right, each of
# them once, and then the call. Spinel hands the operands of most calls to
# one C call, whose order C leaves open (gcc takes them right to left), and
# binds them in order first where it sees that the order shows
# (emit_operands_in_order in src/codegen_call.c, and the arms that do it by
# hand). An arm that does neither answers wrong only for a program whose
# operands have effects, and few programs of the suite have two in one call,
# so the suite does not say which arms are left.
#
# The probe gives every operand an effect. It writes each FILE (default
# test/*.rb) again with every operand `e` of every call spelled `__opN(e)`,
# N the operand's number in the program, and `def __opN(v)` a method that
# logs N to stderr and answers v: a user method called with one argument,
# which is what the compiler sees. It runs that program under ruby and under
# spinel and reads the two logs call by call.
#
# An operand is the receiver or a positional argument. One the compiler reads
# by its spelling is left as written (a constant, self, a symbol, nil, true,
# false, a range, regexp, hash or lambda literal), and so is the whole call
# when it has fewer than two operands left, only literals, a splat or
# keywords, or a name the compiler resolves from the arguments (AS_WRITTEN).
# The body of a block, a lambda or a def is asked on its own: it runs when it
# is called, not when the operand it is written in does.
#
# A call is a finding, classed:
#   count   one of its operands ran another number of times than the others,
#           against ruby's counts: dropped, or run twice
#   order   an operand was logged with the one before it still to run
#   nested  an operand of a call written inside one of its operands was
#           logged before the operand ahead of that one
# Two logs that differ with no such call are a `sequence`, listed and not
# counted: most often a builtin that calls its block in another sequence
# than ruby's does, which ruby does not promise.
#
# The instrumented program has to print under ruby what the program prints
# (its .expected file, or ruby's answer for the file as written), or it is
# left out. When spinel then refuses it, does not build it, or runs it to
# another answer, the question was not asked of that program: it is listed
# under what happened, since the wrap is a valid program, and not counted.
#
# The first programs of every class and method (ALONE of them) are asked
# again with only the operands of that one call wrapped; the summary says
# for how many of them the call is still a finding.
#
# Output, under DIR (default build/operand-probe): summary.txt and, for each
# program with a finding, findings/<n>-<program>/ with probe.rb (the program
# as it was asked), calls.txt and alone-<line>-<column>.rb for a call asked alone.
#
# A probe to run by hand, like call_binding_probe: not a gate, and not one of
# the tools make builds. It needs Prism, which Ruby 3.3 and later bundle.
#
# Exit status: 0 no finding, 1 at least one, 4 the tool's own error.

require "fileutils"
require "prism"
require "rbconfig"
require "tmpdir"
require_relative "probe_common"

module OperandProbe
  NAME = "operand_probe".freeze
  # most severe first
  CLASSES = %w[count order nested].freeze
  # what became of a program the question was not asked of, wrong ones first
  OTHERS = { "answer" => "another answer", "nobuild" => "C that does not build",
             "crash" => "a crash", "timeout" => "out of time",
             "raised" => "an exception ruby does not raise", "refused" => "refused" }.freeze
  # the events a run logs: a loop of a million calls says what its first say
  EVENTS = 100_000
  # the programs of one class and method asked again with one call wrapped
  ALONE = 3
  # the calls of one site kept open at a time: one whose operand raised
  # never ends
  OPEN = 64
  # the compiler resolves these from the spelling of their arguments
  AS_WRITTEN = %w[attr_reader attr_writer attr_accessor require require_relative load autoload
                  private public protected module_function private_constant private_class_method
                  public_class_method include extend prepend using refine define_method
                  alias_method send __send__ public_send method instance_method respond_to?
                  instance_variable_get instance_variable_set const_get is_a? kind_of?
                  instance_of? raise fail catch throw].freeze
  # an operand of one of these kinds is left as written
  PLAIN = [Prism::ConstantReadNode, Prism::ConstantPathNode, Prism::SelfNode, Prism::SymbolNode,
           Prism::NilNode, Prism::TrueNode, Prism::FalseNode, Prism::RangeNode, Prism::HashNode,
           Prism::LambdaNode, Prism::RegularExpressionNode,
           Prism::InterpolatedRegularExpressionNode].freeze
  # a call with one of these among its arguments is left as written
  SPREAD = [Prism::SplatNode, Prism::KeywordHashNode, Prism::ForwardingArgumentsNode].freeze
  # a call of nothing but these is one the compiler may fold
  LITERAL = [Prism::IntegerNode, Prism::FloatNode, Prism::RationalNode, Prism::ImaginaryNode,
             Prism::StringNode].freeze

  class ParseError < StandardError; end

  # One wrapped operand: its number, its bytes in the program, its call and
  # its place among that call's wrapped operands (last for the last of
  # them). inner lists the operands of other calls it is written in, as
  # [call, wrapped operands of that call ahead of it], where there are any
  # ahead and any still to run.
  Site = Struct.new(:id, :from, :to, :call, :pos, :last, :inner)

  # One call with two wrapped operands or more.
  Call = Struct.new(:index, :name, :line, :column, :text, :sites) do
    def at = "#{line}-#{column}"
  end

  # The calls of a program and their sites, and the program written again.
  class Sites
    attr_reader :calls, :sites

    def initialize(source)
      result = Prism.parse(source)
      raise ParseError unless result.errors.empty?
      @source = source
      @calls = []
      @sites = []
      # the methods go in front of the first statement, on its line: every
      # line of the program keeps its number
      @head = result.value.statements.body.first&.location&.start_offset
      walk(result.value, [])
    end

    # The program with `sites` wrapped.
    def write(sites)
      opens = Hash.new { |h, k| h[k] = [] }
      closes = Hash.new(0)
      sites.each do |s|
        opens[s.from] << s
        closes[s.to] += 1
      end
      out = +""
      last = 0
      (opens.keys | closes.keys | [@head]).sort.each do |at|
        out << @source.byteslice(last, at - last) << ")" * closes[at]
        out << methods(sites) if at == @head
        # the widest of the operands that start here is the outermost
        opens[at].sort_by { |s| -s.to }.each { |s| out << "__op#{s.id}(" }
        last = at
      end
      out << @source.byteslice(last..)
    end

    private

    def methods(sites)
      log = "$__op_left = #{EVENTS}; def __op_log(n); if $__op_left > 0; $__op_left -= 1; " \
            "STDERR.puts(\"@@\#{n}\"); end; nil; end; "
      log + sites.map { |s| "def __op#{s.id}(v); __op_log(#{s.id}); v; end; " }.join
    end

    # `within` is what a site under `node` is written in (Site#inner).
    def walk(node, within)
      case node
      when nil then nil
      when Prism::DefNode then walk(node.body, [])
      when Prism::BlockNode, Prism::LambdaNode then node.compact_child_nodes.each { |n| walk(n, []) }
      # a pattern is not a call
      when Prism::InNode then walk(node.statements, within)
      when Prism::MatchPredicateNode, Prism::MatchRequiredNode then walk(node.value, within)
      when Prism::CallNode then call(node, within)
      else node.compact_child_nodes.each { |n| walk(n, within) }
      end
    end

    def call(node, within)
      args = node.arguments ? node.arguments.arguments : []
      operands = [node.receiver, *args].compact
      wrap = wrapped(node, args, operands)
      this = nil
      unless wrap.empty?
        loc = node.location
        this = Call.new(@calls.size, node.name.to_s, loc.start_line, loc.start_column,
                        loc.slice.gsub(/\s+/, " "), [])
        @calls << this
      end
      ahead = 0
      operands.each do |operand|
        # an operand after the last wrapped one has none of the call's to wait for
        inside = this && ahead.positive? && ahead < wrap.size ? within + [[this.index, ahead]] : within
        if wrap.any? { |w| w.equal?(operand) }
          loc = operand.location
          site = Site.new(@sites.size + 1, loc.start_offset, loc.end_offset, this.index, ahead, false, within)
          @sites << site
          this.sites << site
          ahead += 1
        end
        walk(operand, inside)
      end
      this.sites.last.last = true if this
      walk(node.block, within)
    end

    # The operands of a call to wrap; none for a call left as written.
    def wrapped(node, args, operands)
      return [] if AS_WRITTEN.include?(node.name.to_s) || args.any? { |a| SPREAD.any? { |k| a.is_a?(k) } }
      wrap = operands.reject { |o| PLAIN.any? { |k| o.is_a?(k) } }
      return [] if wrap.size < 2 || wrap.all? { |o| LITERAL.any? { |k| o.is_a?(k) } }
      wrap
    end
  end

  module_function

  # The calls a log shows out of Ruby's order: {call index => "order" or
  # "nested"}. A call of a site is open from its first operand to its last,
  # waiting for the next; calls of one site nest (a recursion) and overlap
  # (two threads), so an operand continues any open call that waits for it.
  def disorder(sites, log)
    waits = Hash.new { |h, k| h[k] = [] }
    bad = {}
    log.each do |id|
      site = sites[id - 1] or next
      site.inner.each { |call, ahead| bad[call] ||= "nested" unless waits[call].include?(ahead) }
      open = waits[site.call]
      if site.pos.zero?
        open << 1
        open.shift if open.size > OPEN
      elsif (i = open.rindex(site.pos))
        site.last ? open.delete_at(i) : open[i] += 1
      else
        bad[site.call] = "order"
      end
    end
    bad
  end

  # The calls one operand of which ran another number of times than the
  # others, measured against ruby's log: a block spinel calls more often
  # than ruby moves every operand of a call in it by the same number.
  def miscounted(calls, want, got)
    w = want.tally
    g = got.tally
    calls.select { |c| c.sites.map { |s| g.fetch(s.id, 0) - w.fetch(s.id, 0) }.uniq.size > 1 }.map(&:index)
  end

  # The calls of `prog` that are findings by the two logs, {call index =>
  # class}. A call ruby's own log shows out of order (a retry, a throw out
  # of an operand) is not one the logs can be read for.
  def judge(prog, want, got)
    return {} if want == got
    model = disorder(prog.sites, want)
    found = disorder(prog.sites, got).reject { |call, _| model.key?(call) }
    # a log cut at EVENTS ends inside a call
    if [want, got].all? { |log| log.size < EVENTS }
      miscounted(prog.calls, want, got).each { |call| found[call] = "count" unless model.key?(call) }
    end
    found
  end

  # One run of a program: its stdout, whether it left with success, what
  # became of it otherwise ("timeout", "crash") and its operand log; torn
  # when two threads wrote a line of the log at once.
  Run = Struct.new(:out, :ok, :state, :log, :torn)

  # A program with a finding: the calls, each [Call, class, its operands as
  # ruby's log has them, as spinel's has them], the worst class among them
  # and the program as it was asked. alone is {call index => [text asked
  # alone, still a finding]}.
  Finding = Struct.new(:path, :prog, :calls, :klass, :text, :alone, :dir)

  class Probe
    attr_reader :findings

    def initialize(spinel, ruby, root, work, timeout:, flags:, keep:)
      @spinel = spinel
      @ruby = ruby
      @root = root
      @work = work
      @timeout = timeout
      @flags = flags
      @keep = keep
      @findings = []
      @sequences = []
      @others = Hash.new { |h, k| h[k] = [] }
      @skips = Hash.new { |h, k| h[k] = [] }
      @asked = [0, 0, 0] # programs, calls, operands
      @lock = Mutex.new
      @stopped = false
      @halt = -> { @stopped }
    end

    # Stops the probe: every process in flight is killed, and it and every
    # later one raise ProbeCommon::Stopped.
    def stop
      @stopped = true
    end

    # Waits for `threads` that run programs; an interrupt stops them first.
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

    # Runs argv, its stdout and stderr under `base`.
    def run(argv, base)
      status, timed_out = ProbeCommon.run_timed(argv, @timeout, "#{base}.out", "#{base}.err", @halt)
      state = if timed_out then "timeout"
              elsif status.signaled? then "crash"
              end
      said = File.binread("#{base}.err")
      log = said.scan(/^@@(\d+)$/).map { |(n)| n.to_i }
      Run.new(File.binread("#{base}.out"), !timed_out && status.success?, state, log, said.scan("@@").size != log.size)
    end

    # Builds `src` with spinel and runs it. state is nil for a program that
    # ran to its end, whatever it answered.
    def spinel(src, args)
      bin = "#{src}.bin"
      built = run([@spinel, src, "-o", bin, *@flags], "#{src}.build")
      unless built.ok
        said = File.binread("#{src}.build.err") + built.out
        refused = said.match?(/^spinel: \d+ refusals?, nothing written/)
        return Run.new("", false, built.state || (refused ? "refused" : "nobuild"), [])
      end
      run([bin, *args], "#{src}.run")
    ensure
      FileUtils.rm_f(bin)
    end

    # Asks the program with `sites` wrapped, as dir/<tag>/<its own name>.
    # Answers the text asked and "asked" with the two logs, "left out" with
    # the reason, or what became of spinel's run.
    def ask(path, prog, sites, dir, tag, want, args)
      FileUtils.mkdir_p(File.join(dir, tag))
      src = File.join(dir, tag, File.basename(path))
      text = prog.write(sites)
      File.binwrite(src, text)
      ruby = run([@ruby, "--enable-frozen-string-literal", src, *args], "#{src}.ruby")
      return [text, "left out", nil] if ruby.state || ruby.out != want
      got = spinel(src, args)
      state = got.state
      state ||= (ruby.ok && !got.ok ? "raised" : "answer") if got.out != want || got.ok != ruby.ok
      return [text, state] if state
      return [text, "left out", "threads write the log at once"] if ruby.torn || got.torn
      [text, "asked", ruby.log, got.log]
    end

    # What the program prints: its .expected file, or ruby's answer.
    def reference(path, dir, args)
      return File.binread("#{path}.expected") if File.exist?("#{path}.expected")
      FileUtils.mkdir_p(dir)
      ruby = run([@ruby, "--enable-frozen-string-literal", File.expand_path(path), *args], File.join(dir, "ref"))
      ruby.state ? nil : ruby.out
    end

    # Why ruby does not print `want` for the instrumented program: with an
    # .expected file another ruby wrote, it may not for the program as
    # written either.
    def why_ruby_differs(path, dir, want, args)
      if File.exist?("#{path}.expected")
        ruby = run([@ruby, "--enable-frozen-string-literal", File.expand_path(path), *args], File.join(dir, "asis"))
        return "this ruby does not print the program's .expected" if ruby.state || ruby.out != want
      end
      "ruby answers otherwise for the instrumented program"
    end

    # One program. `id` names its scratch directory.
    def check(path, id)
      prog = begin
        Sites.new(File.binread(path))
      rescue ParseError
        return skip(path, "ruby #{RUBY_VERSION} does not parse it")
      end
      return skip(path, "no call with two operands to wrap") if prog.calls.empty?
      return skip(path, "reads standard input") if File.exist?("#{path}.stdin")
      dir = File.join(@work, id.to_s)
      args = File.exist?("#{path}.args") ? File.read("#{path}.args").split : []
      want = reference(path, dir, args) or return skip(path, "ruby does not run it to its end")
      text, state, *logs = ask(path, prog, prog.sites, dir, "all", want, args)
      return skip(path, logs.first || why_ruby_differs(path, dir, want, args)) if state == "left out"
      if state != "asked"
        # what spinel makes of the program as written is not the wrap's doing
        FileUtils.mkdir_p(File.join(dir, "asis"))
        asis = File.join(dir, "asis", File.basename(path))
        FileUtils.cp(path, asis)
        plain = spinel(asis, args)
        return skip(path, "spinel does not answer as ruby does for the program as written") if plain.state || plain.out != want
        return @lock.synchronize { @others[state] << path }
      end
      found = OperandProbe.judge(prog, *logs)
      @lock.synchronize do
        @asked[0] += 1
        @asked[1] += prog.calls.size
        @asked[2] += prog.sites.size
        if !found.empty?
          calls = found.sort.map do |call, klass|
            [prog.calls[call], klass, *logs.map { |log| places(prog.calls[call], log) }]
          end
          klass = CLASSES.find { |k| found.value?(k) }
          @findings << Finding.new(path, prog, calls, klass, text, {})
        elsif logs[0] != logs[1]
          @sequences << path
        end
      end
    ensure
      FileUtils.rm_rf(dir) if dir && !@keep
    end

    # The findings by class and method, each list in the order of its paths.
    def families
      by = Hash.new { |h, k| h[k] = [] }
      @findings.each do |f|
        f.calls.map { |call, klass| [klass, call.name] }.uniq.each { |key| by[key] << f }
      end
      by.sort_by { |(klass, name), fs| [CLASSES.index(klass), -fs.size, name] }
    end

    # The (finding, call) pairs to ask alone: the first call of each class
    # and method in its first ALONE programs.
    def alone_plan
      families.flat_map do |(klass, name), fs|
        fs.first(ALONE).map { |f| [f, f.calls.find { |call, k| k == klass && call.name == name }.first] }
      end
    end

    # Asks one call alone: its own operands wrapped, and the ones written in
    # them. `id` names the scratch directory.
    def check_alone(f, call, id)
      sites = f.prog.sites.select { |s| s.call == call.index || s.inner.any? { |c, _| c == call.index } }
      dir = File.join(@work, "alone-#{id}")
      args = File.exist?("#{f.path}.args") ? File.read("#{f.path}.args").split : []
      want = reference(f.path, dir, args)
      text, state, *logs = ask(f.path, f.prog, sites, dir, "one", want, args)
      still = state == "asked" && OperandProbe.judge(f.prog, *logs).key?(call.index)
      @lock.synchronize { f.alone[call.index] = [text, still] }
    ensure
      FileUtils.rm_rf(dir) if dir && !@keep
    end

    # The finding directories.
    def write_findings(out)
      @findings.each_with_index do |f, i|
        f.dir = File.join(out, "findings", format("%03d-%s", i + 1, File.basename(f.path, ".rb")))
        FileUtils.mkdir_p(f.dir)
        File.binwrite(File.join(f.dir, "probe.rb"), f.text)
        File.write(File.join(f.dir, "calls.txt"), calls_text(f))
        f.alone.each do |call, (text, _)|
          File.binwrite(File.join(f.dir, "alone-#{f.prog.calls[call].at}.rb"), text)
        end
      end
    end

    # The operands of a call as a log has them, by their place in the call,
    # for as far as `n` events.
    def places(call, log, n = 12)
      place = call.sites.to_h { |s| [s.id, s.pos + 1] }
      shown = log.filter_map { |id| place[id] }
      shown.first(n).join(" ") + (shown.size > n ? " .." : "")
    end

    def calls_text(f)
      s = [relative(f.path)]
      f.calls.each do |call, klass, ruby, spinel|
        s << "" << "line #{call.line}, column #{call.column}  #{call.name}  #{klass}#{alone_note(f, call)}"
        s << "    #{call.text[0, 200]}"
        s << "    ruby    #{ruby}" << "    spinel  #{spinel}"
      end
      s.join("\n") + "\n"
    end

    def alone_note(f, call)
      _, still = f.alone[call.index]
      return "" if still.nil?
      "  (#{still ? "a" : "not a"} finding alone: alone-#{call.at}.rb)"
    end

    def summary(given, out)
      s = ["programs: #{given} given, #{@asked[0]} asked (#{@asked[1]} calls, #{@asked[2]} operands)"]
      unless @skips.empty?
        s << "left out:"
        @skips.sort_by { |why, ps| [-ps.size, why] }.each { |why, ps| s << format("  %5d  %s", ps.size, why) }
      end
      unless @others.empty?
        s << "not asked, the instrumented program being:"
        OTHERS.each { |state, what| s << format("  %5d  %s", @others[state].size, what) if @others.key?(state) }
      end
      s << "" << "findings: #{@findings.size} programs, #{@findings.sum { |f| f.calls.size }} calls"
      CLASSES.each do |k|
        fs = @findings.select { |f| f.calls.any? { |_, klass| klass == k } }
        next if fs.empty?
        s << format("  %-7s %5d programs %6d calls", k, fs.size, fs.sum { |f| f.calls.count { |_, klass| klass == k } })
      end
      unless @findings.empty?
        s << "" << "by class and method (programs, calls, and of the programs asked alone how many still show it):"
        families.each do |(klass, name), fs|
          calls = fs.sum { |f| f.calls.count { |call, k| k == klass && call.name == name } }
          tried = fs.filter_map { |f| f.alone[f.calls.find { |call, k| k == klass && call.name == name }.first.index] }
          first = fs.first.calls.find { |call, k| k == klass && call.name == name }.first
          s << format("  %-7s %-14s %5d %6d   alone %d of %d   %s:%d  %s", klass, name, fs.size, calls,
                      tried.count { |_, still| still }, tried.size, relative(fs.first.path), first.line, first.text[0, 60])
        end
        CLASSES.each do |k|
          fs = @findings.select { |f| f.klass == k }
          next if fs.empty?
          s << "" << "#{k}:"
          fs.each do |f|
            call, = f.calls.find { |_, klass| klass == k }
            more = f.calls.size > 1 ? "  (and #{f.calls.size - 1} more)" : ""
            s << "  #{relative(f.path)}:#{call.line}  #{call.name}#{more}  #{f.dir.delete_prefix("#{out}/")}"
          end
        end
      end
      unless @sequences.empty?
        s << "" << "sequence (the logs differ and no call's operands are out of order; not counted):"
        @sequences.sort.each { |p| s << "  #{relative(p)}" }
      end
      OTHERS.each_key do |state|
        next if %w[raised refused].include?(state) || !@others.key?(state)
        s << "" << "#{OTHERS[state]}, instrumented (not counted):"
        @others[state].sort.each { |p| s << "  #{relative(p)}" }
      end
      s.join("\n") + "\n"
    end
  end

  USAGE = "usage: ruby tools/#{NAME}.rb [--int-overflow promote] [--jobs J] [--out DIR] " \
          "[--timeout SEC] [--keep] [FILE..]".freeze

  # Runs `work` over `items` on `jobs` threads, with a progress line.
  def each_in_parallel(probe, items, jobs, what)
    queue = Queue.new
    items.each_with_index { |item, i| queue << [item, i] }
    done = 0
    progress = Mutex.new
    workers = Array.new(jobs) do
      Thread.new do
        while (item, i = (queue.pop(true) rescue nil))
          begin
            yield item, i
          rescue ProbeCommon::Stopped
            break
          end
          progress.synchronize do
            done += 1
            $stderr.print "\r#{done}/#{items.size} #{what}, #{probe.findings.size} with findings"
          end
        end
      end
    end
    probe.finish(workers)
    $stderr.puts
  end

  # The command line. Answers the exit status.
  def main(argv)
    root = File.expand_path("..", __dir__)
    jobs = 4
    timeout = 30
    keep = false
    promote = false
    out = File.join(root, "build", "operand-probe")
    files = []
    args = argv.dup
    begin
      until args.empty?
        case (arg = args.shift)
        when "--jobs" then jobs = Integer(args.shift)
        when "--timeout" then timeout = Integer(args.shift)
        when "--out" then out = File.expand_path(args.shift || raise(ArgumentError))
        when "--int-overflow" then promote = args.shift == "promote" || raise(ArgumentError)
        when "--keep" then keep = true
        when /\A--/ then raise ArgumentError
        else files << arg
        end
      end
      raise ArgumentError unless [jobs, timeout].all?(&:positive?)
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
    files = Dir.glob(File.join(root, "test/*.rb")) if files.empty?
    missing = files.reject { |f| File.file?(f) }
    unless missing.empty?
      warn "#{NAME}: no such file: #{missing.first}"
      return 4
    end
    # the promote tests are another language level: they compile, and mean
    # what they print, only under the flag
    files = files.select { |f| File.basename(f).start_with?("promote_") == promote } unless files.size == 1
    files = files.map { |f| File.expand_path(f) }.sort
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
      work = keep ? File.join(out, "work") : Dir.mktmpdir("operand-probe")
      FileUtils.mkdir_p(work)
      # the programs name their files from the repository's root
      Dir.chdir(root)
      probe = Probe.new(spinel, ENV["RUBY"] || RbConfig.ruby, root, work, timeout: timeout, keep: keep,
                                                                         flags: promote ? ["--int-overflow=promote"] : [])
      # A compiler that does not build a two-line program, or that runs its
      # operands as ruby does where master does not, is the tool's failure.
      pre = File.join(work, "preflight.rb")
      File.write(pre, "def one(v) = v\np [3, 1, 2].each_slice(one(2)).to_a\n")
      first = nil
      probe.finish([Thread.new { first = probe.spinel(pre, []) }])
      raise "#{spinel} does not build and run a two-line program (#{first.state || first.out.inspect})" unless first.out == "[[3, 1], [2]]\n"

      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      each_in_parallel(probe, files, jobs, "programs") do |f, i|
        probe.check(f, i)
      rescue ProbeCommon::Stopped
        raise
      rescue StandardError => e
        # one program the tool cannot read is one program left out
        probe.skip(f, "tool error: #{e.class}: #{e.message.lines.first.to_s.strip}")
      end
      probe.findings.sort_by! { |f| [CLASSES.index(f.klass), probe.relative(f.path)] }
      each_in_parallel(probe, probe.alone_plan, jobs, "calls alone") { |(f, call), i| probe.check_alone(f, call, i) }
      took = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
      probe.write_findings(out)
      version = IO.popen([spinel, "--version"], err: File::NULL, &:read).strip
      report = "spinel: #{version}\nruby: #{RUBY_DESCRIPTION}\n" +
               probe.summary(files.size, out) + format("\nwall time: %ds (jobs %d)\n", took, jobs)
      File.write(File.join(out, "summary.txt"), report)
      puts report
      puts "findings under #{out}" unless probe.findings.empty?
      probe.findings.empty? ? 0 : 1
    rescue StandardError => e
      warn "#{NAME}: #{e.message}"
      4
    ensure
      FileUtils.rm_rf(work) if work && !keep
      dir_lock.close
    end
  end
end

exit OperandProbe.main(ARGV) if $PROGRAM_NAME == __FILE__
