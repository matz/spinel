# Inference order probe: sibling definitions reordered, the types compared.
#
#   ruby tools/order_probe.rb [--strategy reverse|rotate|shuffle|swap]..
#                             [--seed S] [--control null|shift|ids]
#                             [--int-overflow promote] [--jobs J] [--out DIR]
#                             [--timeout SEC] [--keep] [FILE..]
#
# Spinel settles the types of a whole program at once, and a method's place
# among its siblings is not supposed to be part of the answer. The probe
# asks whether it is. It takes each FILE (default test/*.rb test/infer/*.rb
# benchmark/*.rb), finds the runs of sibling `def`s (tools/order_permute.rb),
# writes the program again with each run in another order, compiles both
# with --emit-types, and compares the type of every node through the line map
# (tools/order_types.rb). A slot typed in one order and boxed in the other
# prints the same answer from both binaries, so no test of the suite sees
# it; the type dump does.
#
# A strategy names the other order: `reverse` (the default) and `rotate` and
# a seeded `shuffle` move every run at once, `swap` exchanges two definitions
# of one run at a time (the 40 nearest pairs of a program). A control
# changes the text without changing the order and has to find nothing:
# `null` writes the file order through the same writer, `shift` adds a
# comment line inside every run, `ids` adds a statement at the top, which
# renumbers the compiler's nodes. A program whose types move under `ids` is
# left out of a run's findings as id-sensitive: what moved them was not the
# order.
#
# The differences of a program are grouped under root slots, named without
# line numbers (`Rng#next_u32`, `Rng@s0`), and classed: `outcome` (one order
# is refused or crashes the compiler), `disagreement` (two types, neither a
# boxed form of the other), `precision` (one order boxes what the other
# types), `shape` (a slot one order does not have), and, listed without
# counting against the exit status, `representation` (one RBS type, two
# internal ones) and `instantiation` (inside a method that yields, which is
# typed once per call site: docs/limitations.md).
#
# Output, under DIR (default build/order-probe): summary.txt and, for each
# finding, findings/<n>-<program>/ with a.rb (the file order), b.rb (the
# other one) and slots.txt.
#
# A probe to run by hand, like call_binding_probe: not a gate, and not one of
# the tools make builds.
#
# Exit status: 0 no finding that counts, 1 at least one, 4 the tool's own
# error.

require "fileutils"
require "json"
require "rbconfig"
require "tmpdir"
require_relative "order_permute"
require_relative "order_types"
require_relative "probe_common"

module OrderProbe
  NAME = "order_probe".freeze
  # most severe first; the first four are what exit status 1 reports
  CLASSES = %w[outcome disagreement precision shape representation instantiation].freeze
  COUNTED = CLASSES.first(4).freeze
  # The permutations of one program a strategy may give, which only `swap`
  # reaches, neighbours first: one test has a run of 1,030 definitions,
  # which is half a million pairs.
  PERMUTATIONS = 40
  # what changes the text spinel parses or the dump it writes
  # (src/spinel_parse.c): the two orders have to be compiled alike
  PINNED = %w[SPINEL_NO_BUILTINS SPINEL_DEBUG SPINEL_LINE_MAP SPINEL_REQUIRE_GATE
              SPINEL_EMIT_TYPES SPINEL_WARN_WIDEN].to_h { |v| [v, nil] }
                                                 .merge("SP_FIXPOINT_LOG" => "1").freeze

  # One compile. state is "ok", "refused" (spinel said no), "crashed" (a
  # signal), "timeout" or "no-json" (exit 0 and no dump); doc the parsed
  # dump; rounds the fixpoint's round count as spinel logs it; message the
  # first line of what it said, with node numbers folded.
  Compile = Struct.new(:state, :doc, :rounds, :message)

  # A program whose types follow the order. roots are the root slots that
  # differ (OrderTypes::Root), klass the most severe class among them and
  # root the one the finding is named by: an instance variable before a
  # parameter before a method, since the first is most often what the others
  # follow. label names the permutation that showed it, perm is that
  # permutation, a and b the two texts compiled, shown how many of the
  # permutations tried show a difference.
  Finding = Struct.new(:path, :klass, :root, :roots, :label, :perm, :a, :b, :rounds, :shown, :dir) do
    def pair = root.pair.map { |type| type || "(no record)" }.join("  /  ")
  end

  class Probe
    attr_reader :findings

    def initialize(spinel, root, work, timeout:, flags:, plan:, control:, keep:)
      @spinel = spinel
      @root = root
      @work = work
      @timeout = timeout
      @flags = flags
      @plan = plan # [[label, ->(source, runs, path) { [Permuted] }], ..]
      @keep = keep
      @control = control # the plan changes the text and not the order: it has to find nothing
      @findings = []
      @skips = Hash.new { |h, k| h[k] = [] }
      @capped = [] # the programs with more pairs than `swap` was given
      @rejects = Hash.new(0)
      @id_sensitive = []
      @compiles = 0
      @probed = 0
      @lock = Mutex.new
      @stopped = false
      @halt = -> { @stopped }
    end

    # Stops the probe: every compile in flight is killed, and it and every
    # later one raise ProbeCommon::Stopped.
    def stop
      @stopped = true
    end

    # Waits for `threads` that compile. An interrupt, which reaches the main
    # thread only, stops them first: a compile runs in a process group of
    # its own, which the terminal's interrupt does not reach, and it would
    # go on writing under a work directory the run has removed.
    def finish(threads)
      threads.each(&:join)
    ensure
      if threads.any?(&:alive?)
        stop
        threads.each(&:join)
      end
    end

    # Compiles `src` to its type dump. The dump of a run before is taken out
    # of the way first: a compile that dies before it writes would leave it.
    def compile(src, json)
      FileUtils.rm_f(json)
      log = "#{json}.log"
      argv = [PINNED, @spinel, src, "--emit-types", "-o", json, *@flags]
      status, timed_out = ProbeCommon.run_timed(argv, @timeout, log, log, @halt)
      @lock.synchronize { @compiles += 1 }
      said = File.exist?(log) ? File.binread(log) : ""
      rounds = said[/^\[fp\] rounds=(\d+(?: \(CAP\))?)/, 1]
      message = said.lines.reject { |l| l.start_with?("[fp]", "Wrote ") }.first.to_s.strip
                    .delete_prefix("#{File.dirname(src)}/").gsub(/\bnode \d+/, "node N")
      doc = (OrderTypes.read(json) rescue nil) if File.exist?(json)
      state = if timed_out then "timeout"
              elsif status.signaled? then "crashed"
              elsif !status.success? then "refused"
              elsif doc.nil? then "no-json"
              else "ok"
              end
      Compile.new(state, doc, rounds, message)
    end

    # The permutations of one program, each text once: two strategies that
    # write the same order are one compile and one count.
    def permutations(source, runs, path)
      seen = { OrderPermute.apply(source, runs, {}).text => true }
      @plan.flat_map do |label, make|
        # by the path under the repository: a shuffle is the same in every checkout
        made = make.call(source, runs, relative(path))
        made.filter_map do |perm|
          next if seen[perm.text] && !@control
          seen[perm.text] = true
          [exchange(runs, perm) || label, perm]
        end
      end
    end

    # A permutation that exchanges two definitions of one run, by their names.
    def exchange(runs, perm)
      return nil unless perm.orders.size == 1
      index, order = perm.orders.first
      moved = order.each_index.reject { |k| order[k] == k }
      return nil unless moved.size == 2
      run = runs.find { |r| r.index == index }
      names = moved.map { |k| "#{run.units[k].receiver && "#{run.units[k].receiver}."}#{run.units[k].name}" }
      "swap #{names.join(" and ")}#{run.owner.empty? ? "" : " of #{run.owner}"}"
    end

    def skip(path, why)
      @lock.synchronize { @skips[why] << path }
    end

    # What a compile of the scratch copy has to share with a compile of the
    # program where it lives: a program that reads a file beside itself
    # (require_relative) is another program in the scratch directory.
    def relocatable?(there, here)
      return false unless there.state == here.state
      return true unless there.state == "ok"
      shape = ->(doc) { doc["types"].map { |r| r.values_at("line", "col", "kind", "name", "type") }.tally }
      there.doc["types"].size == here.doc["types"].size && shape.(there.doc) == shape.(here.doc)
    end

    # One program: the file order and each permutation of it, compiled and
    # compared. `id` names its scratch directory.
    def check(path, id)
      source = File.binread(path)
      begin
        runs = OrderPermute.runs(source)
        facts = OrderPermute.facts(source)
      rescue OrderPermute::ParseError
        return skip(path, "ruby #{RUBY_VERSION} does not parse it")
      end
      @lock.synchronize { runs.each { |r| @rejects[r.reject] += 1 if r.reject } }
      # a control reads the programs a permutation would, and no others
      return skip(path, "no run of definitions to reorder") if runs.all?(&:reject)
      perms = permutations(source, runs, path)
      return skip(path, "no other order under the strategies given") if perms.empty?
      pairs = runs.reject(&:reject).sum { |run| run.units.size * (run.units.size - 1) / 2 }
      @lock.synchronize { @capped << path } if pairs > PERMUTATIONS && @plan.any? { |label, _| label == "swap" }
      dir = File.join(@work, id.to_s)
      base = File.basename(path)
      a_text = OrderPermute.apply(source, runs, {}).text
      a_path = write(dir, "a", base, a_text)
      a = compile(a_path, "#{a_path}.json")
      unless relocatable?(compile(File.expand_path(path), File.join(dir, "o.json")), a)
        return skip(path, "not relocatable (it compiles to something else outside its directory)")
      end
      compared = false
      found = perms.each_with_index.filter_map do |(label, perm), i|
        b_path = write(dir, "b#{i}", base, perm.text)
        b = compile(b_path, "#{b_path}.json")
        roots, why = compare(a, b, a_path, b_path, facts, runs, perm.line_map)
        why ? skip(path, why) : compared = true
        next if roots.empty?
        top = roots.min_by { |r| [CLASSES.index(r.klass), rank(r.name), r.name] }
        Finding.new(path, top.klass, top, roots, label, perm, a_text, perm.text, [a.rounds, b.rounds])
      end
      # the permutation that shows the worst of it, and the most
      best = found.min_by { |f| [CLASSES.index(f.klass), -f.roots.size] }
      best.shown = "#{found.size} of #{perms.size}" if best
      if best && !@control && best.klass != "outcome" && id_sensitive?(source, runs, facts, dir, base, a, a_path)
        @lock.synchronize { @id_sensitive << path }
        best = nil
      end
      @lock.synchronize do
        @probed += 1 if compared
        @findings << best if best
      end
    ensure
      FileUtils.rm_rf(dir) if dir && !@keep
    end

    # Which root names a finding, among those of one class.
    def rank(name)
      if name.include?("@") then 0
      elsif name.end_with?(")") && !name.start_with?("(") && !name.end_with?("(body)") then 1
      elsif name.match?(/[#.]/) then 2
      else name.start_with?("(") || name.end_with?("(body)") ? 4 : 3
      end
    end

    # The root slots that differ between two compiles, and nil; or none
    # and why the two could not be compared. One order refused, crashed or
    # out of time is a root of its own, `(compile)`.
    def compare(a, b, a_path, b_path, facts, runs, line_map)
      if a.state != b.state
        said = [a, b].map { |c| c.state == "ok" ? "ok" : "#{c.state}: #{c.message}" }
        return [[OrderTypes::Root.new(name: "(compile)", slots: [], klass: "outcome", pair: said)], nil]
      end
      return [[], "#{a.state} in both orders"] if a.state != "ok"
      res = OrderTypes.compare(a.doc, b.doc, file_a: a_path, file_b: b_path,
                                             facts: facts, runs: runs, line_map: line_map)
      return [[], "unalignable (#{res.reason.sub(/:.*/m, "")})"] if res.status != :ok
      [res.roots.values, nil]
    end

    # Whether the program's types move when only the node numbers do.
    def id_sensitive?(source, runs, facts, dir, base, a, a_path)
      perm = OrderPermute.control(source, runs, "ids")
      c_path = write(dir, "ids", base, perm.text)
      c = compile(c_path, "#{c_path}.json")
      return true unless c.state == "ok" && a.state == "ok"
      res = OrderTypes.compare(a.doc, c.doc, file_a: a_path, file_b: c_path,
                                             facts: facts, runs: runs, line_map: perm.line_map)
      res.status != :ok || !res.roots.empty?
    end

    # Each copy keeps the program's own file name, in a directory of its own:
    # a program that prints __FILE__ prints the same from every copy.
    def write(dir, sub, base, text)
      FileUtils.mkdir_p(File.join(dir, sub))
      File.join(dir, sub, base).tap { |p| File.binwrite(p, text) }
    end

    # The finding directories, and the path each finding is shown by.
    def write_findings(out)
      @findings.each_with_index do |f, i|
        f.dir = File.join(out, "findings", format("%03d-%s", i + 1, File.basename(f.path, ".rb")))
        FileUtils.mkdir_p(f.dir)
        File.binwrite(File.join(f.dir, "a.rb"), f.a)
        File.binwrite(File.join(f.dir, "b.rb"), f.b)
        File.write(File.join(f.dir, "slots.txt"), slots_text(f))
      end
    end

    def slots_text(f)
      s = [relative(f.path), "permutation: #{f.label} (#{f.shown} permutations tried show a difference)",
           "fixpoint rounds: #{f.rounds[0] || "?"} in file order (a.rb), #{f.rounds[1] || "?"} in the other (b.rb)"]
      f.roots.sort_by { |r| [CLASSES.index(r.klass), rank(r.name), r.name] }.each do |root|
        s << "" << "#{root.name}  #{root.klass}  #{root.pair.map { |t| t || "(no record)" }.join("  /  ")}"
        root.slots.each do |slot|
          lost = slot.loser ? " (#{slot.loser}.rb loses)" : ""
          s << "  line #{slot.line}  #{[slot.kind, slot.name].compact.join(" ")}  #{slot.klass}#{lost}"
          s << "      a.rb  #{side(slot.rbs_a, slot.types_a)}" << "      b.rb  #{side(slot.rbs_b, slot.types_b)}"
        end
      end
      s.join("\n") + "\n"
    end

    # One order's types of a slot: the RBS, and the internal tags where there are any.
    def side(rbs, tags)
      return "(no record)" if rbs.empty?
      tags.empty? ? rbs.join(", ") : "#{rbs.join(", ")}  [#{tags.join(", ")}]"
    end

    def relative(path)
      File.expand_path(path).delete_prefix("#{@root}/")
    end

    def summary(given, out)
      s = ["programs: #{given} given, #{@probed} compared in two orders or more, #{@compiles} type compiles"]
      unless @skips.empty?
        s << "left out:"
        @skips.sort_by { |why, ps| [-ps.uniq.size, why] }.each { |why, ps| s << format("  %5d  %s", ps.uniq.size, why) }
      end
      unless @capped.empty?
        s << "swap: #{@capped.size} programs have more pairs than the #{PERMUTATIONS} nearest, which are the ones tried"
      end
      unless @rejects.empty?
        s << "runs not moved: " + @rejects.sort_by { |why, n| [-n, why] }.map { |why, n| "#{n} #{why}" }.join(", ")
      end
      unless @id_sensitive.empty?
        s << "id-sensitive (types that follow the node numbers; not counted):"
        @id_sensitive.sort.each { |p| s << "  #{relative(p)}" }
      end
      s << ""
      by = @findings.group_by(&:klass)
      s << "findings: #{@findings.size} programs, #{@findings.sum { |f| f.roots.size }} root slots"
      CLASSES.each do |k|
        fs = by[k] or next
        s << format("  %-14s %4d%s", k, fs.size, COUNTED.include?(k) ? "" : "  (not counted)")
      end
      CLASSES.each do |k|
        fs = by[k] or next
        s << "" << "#{k}:"
        fs.each do |f|
          more = f.roots.size > 1 ? "  (and #{f.roots.size - 1} more)" : ""
          s << "  #{relative(f.path)}  #{f.root.name}  #{f.pair}#{more}"
          s << "    #{f.dir.delete_prefix("#{out}/")}  #{f.label}"
        end
      end
      s.join("\n") + "\n"
    end
  end

  USAGE = "usage: ruby tools/#{NAME}.rb [--strategy reverse|rotate|shuffle|swap].. [--seed S] " \
          "[--control null|shift|ids] [--int-overflow promote] [--jobs J] [--out DIR] " \
          "[--timeout SEC] [--keep] [FILE..]".freeze

  module_function

  # The command line. Answers the exit status.
  def main(argv)
    root = File.expand_path("..", __dir__)
    strategies = []
    control = nil
    seed = 1
    jobs = 4
    timeout = 30
    keep = false
    promote = false
    out = File.join(root, "build", "order-probe")
    files = []
    args = argv.dup
    begin
      until args.empty?
        case (arg = args.shift)
        when "--strategy" then strategies << ((OrderPermute::STRATEGIES & [args.shift]).first || raise(ArgumentError))
        when "--control" then control = (OrderPermute::CONTROLS & [args.shift]).first || raise(ArgumentError)
        when "--seed" then seed = Integer(args.shift)
        when "--jobs" then jobs = Integer(args.shift)
        when "--timeout" then timeout = Integer(args.shift)
        when "--out" then out = File.expand_path(args.shift || raise(ArgumentError))
        when "--int-overflow" then promote = args.shift == "promote" || raise(ArgumentError)
        when "--keep" then keep = true
        when /\A--/ then raise ArgumentError
        else files << arg
        end
      end
      raise ArgumentError unless [jobs, timeout].all?(&:positive?) && (control.nil? || strategies.empty?)
    rescue ArgumentError, TypeError => e
      warn "#{NAME}: #{e.message}" unless e.message == "ArgumentError"
      warn USAGE
      return 4
    end
    strategies = ["reverse"] if strategies.empty?
    spinel = File.expand_path(ENV["SPINEL"] || File.join(root, "spinel"))
    unless File.executable?(spinel)
      warn "#{NAME}: no spinel at #{spinel} (build it, or set SPINEL)"
      return 4
    end
    if files.empty?
      files = %w[test/*.rb test/infer/*.rb benchmark/*.rb].flat_map { |g| Dir.glob(File.join(root, g)) }
    end
    missing = files.reject { |f| File.file?(f) }
    unless missing.empty?
      warn "#{NAME}: no such file: #{missing.first}"
      return 4
    end
    # the promote tests are another language level: they compile, and mean
    # what they print, only under the flag
    files = files.select { |f| File.basename(f).start_with?("promote_") == promote } unless files.size == 1
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
      work = keep ? File.join(out, "work") : Dir.mktmpdir("order-probe")
      FileUtils.mkdir_p(work)
      plan = if control
               [[control, ->(source, runs, _path) { [OrderPermute.control(source, runs, control)] }]]
             else
               strategies.uniq.map do |s|
                 [s, lambda do |source, runs, path|
                   OrderPermute.permute(source, runs, s, seed: seed, path: path, limit: PERMUTATIONS)
                 end]
               end
             end
      probe = Probe.new(spinel, root, work, timeout: timeout, flags: promote ? ["--int-overflow=promote"] : [],
                                            plan: plan, control: !control.nil?, keep: keep)
      # A compiler staged without its builtins refuses every program that
      # names an Enumerable method: that is the tool's failure, not a finding.
      pre = File.join(work, "preflight.rb")
      File.write(pre, "p [3, 1, 2].each_slice(2).to_a\n")
      first = nil
      probe.finish([Thread.new { first = probe.compile(pre, "#{pre}.json") }])
      raise "#{spinel} does not compile a one-line program (#{first.state}: #{first.message})" unless first.state == "ok"

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
      probe.findings.sort_by! { |f| [CLASSES.index(f.klass), probe.relative(f.path)] }
      probe.write_findings(out)
      version = IO.popen([spinel, "--version"], err: File::NULL, &:read).strip
      what = control ? "control: #{control}" : "strategies: #{strategies.uniq.join(" ")} (seed #{seed})"
      report = "spinel: #{version}\nruby: #{RUBY_DESCRIPTION}\n#{what}\n" +
               probe.summary(files.size, out) + format("\nwall time: %ds (jobs %d)\n", took, jobs)
      File.write(File.join(out, "summary.txt"), report)
      puts report
      puts "findings under #{out}" unless probe.findings.empty?
      probe.findings.any? { |f| COUNTED.include?(f.klass) } ? 1 : 0
    rescue StandardError => e
      warn "#{NAME}: #{e.message}"
      4
    ensure
      FileUtils.rm_rf(work) if work && !keep
      dir_lock.close
    end
  end
end

exit OrderProbe.main(ARGV) if $PROGRAM_NAME == __FILE__
