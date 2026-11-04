#!/usr/bin/env ruby
# frozen_string_literal: true
# CRuby observations, not a proof over arbitrary Ruby values. Failed calls
# are coverage gaps, never evidence of purity. Run with frozen literals:
# ruby --enable-frozen-string-literal tools/gen_builtin_share_spec.rb --write
# --check regenerates; -o FILE writes the human-readable classification table.
require_relative "gen_builtin_arity_spec"
require "tmpdir"
require "optparse"

module BuiltinShareProbe
  ROOT = File.expand_path("..", __dir__)
  SPEC = File.join(ROOT, "src/builtin_share_spec.inc")
  DATA = File.join(ROOT, "tools/builtin_share_spec.json")
  PURE = File.join(ROOT, "src/builtin_share_pure.inc")
  FAMILIES = {
    "String" => "TY_STRING", "Integer" => "TY_INT", "Float" => "TY_FLOAT",
    "Symbol" => "TY_SYMBOL", "Array" => "BOP_ANY_ARRAY", "Hash" => "BOP_ANY_HASH",
    "Range" => "TY_RANGE", "StringRange" => "TY_STR_RANGE", "Time" => "TY_TIME", "NilClass" => "TY_NIL",
    "TrueClass" => "TY_BOOL", "Rational" => "TY_RATIONAL", "Complex" => "TY_COMPLEX",
    "Object" => "BOP_ANY_RECV", "File" => "TY_IO", "Regexp" => "TY_REGEX",
    "MatchData" => "TY_MATCHDATA", "Random" => "TY_RANDOM", "Class" => "TY_CLASS",
    "Module" => "TY_CLASS", "OpenStruct" => "TY_OPENSTRUCT",
    "BigInteger" => "TY_BIGINT", "FloatRange" => "TY_FLOAT_RANGE",
    "FalseClass" => "TY_BOOL", "Proc" => "BOP_CALLABLE", "Method" => "BOP_CALLABLE",
    "Enumerator" => "BOP_ANY_ARRAY"
  }.freeze
  # These methods can affect the probing process, run external commands, or
  # enter a native wait. They remain explicitly unprobed, even if arity-only
  # probing (which passes nil) can safely exercise them.
  UNSAFE = %w[exit exit! abort fork exec spawn system ` trap syscall sleep select
    kill terminate stop run wakeup join raise fail wait wait2 waitpid waitpid2
    getpriority setpriority setproctitle handle_interrupt
    lock synchronize wait_signal signal broadcast resume transfer yield
    autoload require require_relative load eval instance_eval class_eval module_eval
    send __send__ public_send method_missing remove_instance_variable
    rmtree rmdir unlink delete rename truncate chmod chown lchmod lchown utime
    link symlink mkfifo sysopen for_fd ioctl fcntl reopen close close_read close_write
    pread set_encoding_by_bom set_trace_func trace_var untrace_var
  ].freeze
  # Safe exceptions run only against fresh, private objects or our ENV key.
  SAFE_DELETE = %w[String Array Hash Set ENV OpenStruct].freeze
  BITS = {"receiver" => 1, "element" => 2, "arg0" => 4, "arg1" => 8,
          "arg2" => 16, "block" => 32}.freeze
  class YieldLimit < StandardError; end

  def self.reachable(value, marker, seen = {}, depth = 0)
    return true if value.equal?(marker)
    return false if depth > 4 || seen[value.object_id]
    seen[value.object_id] = true
    children = case value
    when Array then value
    when Hash then value.keys + value.values + [value.default_proc ? value.default(:share_oracle_missing) : value.default]
    when Struct then value.values
    when Set then value.to_a
    when StringIO then [value.string]
    when StringScanner then [value.string]
    when Method then [value.receiver]
    when Range then [value.begin, value.end]
    when Exception then [value.message, value.backtrace]
    when Queue
      items = value.size.times.map { value.pop(true) }
      items.each { |v| value << v }
      items
    when Enumerator then value.take(3)
    else value.instance_variables.map { |v| value.instance_variable_get(v) }
    end
    children.any? { |v| reachable(v, marker, seen, depth + 1) }
  rescue StandardError => e
    @opaque_reads << e.class.name if value.is_a?(Enumerator)
    false
  end

  def self.snapshot(value)
    case value
    when String then [value.dup, value.encoding.name, value.frozen?]
    when Array then value.map(&:object_id)
    when Hash then [value.to_a.map { |k, v| [k.object_id, v.object_id] }, value.default.object_id]
    when Set then value.to_a.map(&:object_id)
    when StringIO then [value.string.dup, value.pos]
    when IO then [value.closed?, (value.pos rescue nil), (value.stat.size rescue nil)]
    when Queue then value.size
    else value.instance_variables.sort.map { |v| [v, value.instance_variable_get(v).object_id] }
    end
  end

  def self.receiver(cls, factory, variant, element)
    case cls
    when "String" then ["ab a\n", "", "AB", "abc", "aaab", "Ééあ", "\xffa".b][variant].dup
    when "Array" then variant == 0 ? [1, 2] : variant == 1 ? [] : [element, element]
    when "Hash" then variant == 0 ? {1 => 2} : variant == 1 ? {} : {:a => element}
    when "Range" then variant == 1 ? (2...2) : (1..2)
    when "StringRange" then variant == 1 ? ("b"..."b") : variant == 2 ? (element..element) : ("a".."b")
    when "Set" then variant == 1 ? Set.new : Set.new([element])
    when "Queue" then Queue.new([element])
    when "SizedQueue" then q = SizedQueue.new(4); q << element; q
    when "File"
      io = factory.call
      if variant == 1
        io.truncate(0)
        io.rewind
      end
      io
    when "Object" then Object.new
    when "OpenStruct" then OpenStruct.new(a: element)
    else factory.call
    end
  end

  def self.shapes(cls, name)
    # Tokens are materialised afresh for every call. Substitution into each
    # position tests String and Object identity independently of other args.
    a = [[], [0], [1], [nil], [:a], ["a"], [/a/], [[1]], [{a: 1}],
         [0, 1], [1, 1], ["a", "b"], [/a/, "b"], [0, 1, "b"],
         [1, 0, "b"], ["UTF-8"], [:@probe, "b"], [Object], [:to_s]]
    a += [["f"], ["f", 1], ["f", "a"], ["f", "r"]] if %w[File Pathname IO].include?(cls)
    a += [["SPINEL_SHARE_ORACLE"], ["SPINEL_SHARE_ORACLE", "b"]] if cls == "ENV"
    a += [[:probe_tag, "b"]] if name == "throw"
    a.uniq
  end

  def self.probe(label, name, factory)
    cls = label.delete_suffix(".")
    class_side = label.end_with?(".")
    @opaque_reads = []
    effects = Array.new(4, 0)
    cases = {}
    witnesses = {}
    ok = 0
    errors = Hash.new(0)
    attempted = 0
    receiver_return = false
    receiver_mutated = false
    modes = %w[String Object]
    variants = !class_side && cls == "String" ? (0..6).to_a :
               !class_side && %w[Array Hash Range StringRange Set File].include?(cls) ? [0, 1, 2] : [0]
    shapes(cls, name).each_with_index do |shape, si|
      ([-1] + (0...shape.length).to_a).each do |position|
        modes.each do |mode|
          variants.each do |variant|
            [false, true].each do |block|
              attempted += 1
              key = [shape.length, block, mode]
              entry = (cases[key] ||= {"argc" => shape.length, "block" => block, "marker" => mode, "ok" => 0, "origins" => 0, "wrapped" => 0, "effects" => [0, 0, 0, 0]})
              markers = {}
              element = mode == "String" ? +"a" : Object.new
              recv = class_side ? factory.call : receiver(cls, factory, variant, element)
              markers["receiver"] = recv if !class_side && (cls == "String" || cls == "Object")
              markers["element"] = element if variant == 2 || %w[Set OpenStruct Queue SizedQueue].include?(cls)
              args = shape.map { |v| Marshal.load(Marshal.dump(v)) rescue v }
              if position >= 0
                marker = mode == "String" ? +"a" : Object.new
                # A key/path/encoding must stay valid when it is the marker.
                marker = shape[position].dup if mode == "String" && shape[position].is_a?(String)
                args[position] = case shape[position]
                when Array then [marker]
                when Hash then {a: marker}
                else marker
                end
                entry["wrapped"] |= BITS.fetch("arg#{position}") unless args[position].equal?(marker)
                markers["arg#{position}"] = marker
              end
              block_value = mode == "String" ? +"block_value" : Object.new
              markers["block"] = block_value if block
              before = markers.transform_values { |v| snapshot(v) }
              recv_before = snapshot(recv)
              yielded = []
              calls = 0
              # Try both a fresh result and the existing memo: returning the
              # memo is necessary to expose inject's argument identity.
              fn = proc do |*xs|
                yielded.concat(xs)
                calls += 1
                raise YieldLimit if calls > 8
                if %w[inject reduce].include?(name)
                  xs[0]
                elsif %w[sort sort! sort_by sort_by! min max minmax bsearch bsearch_index].include?(name)
                  0
                elsif %w[to_h].include?(name)
                  [:a, block_value]
                else
                  block_value
                end
              end
              begin
                result = if name == "throw" && cls == "Kernel"
                  catch(args[0]) { Kernel.__send__(name, *args) }
                else
                  block ? recv.__send__(name, *args, &fn) : recv.__send__(name, *args)
                end
                ok += 1
                entry["ok"] += 1
                entry["origins"] |= markers.keys.sum { |o| BITS.fetch(o) }
                receiver_return ||= result.equal?(recv)
                receiver_mutated ||= snapshot(recv) != recv_before
                markers.each do |origin, marker|
                  bit = BITS.fetch(origin)
                  # An immutable answer cannot participate in later mutable
                  # String aliasing (freeze/-@ and frozen Hash keys).
                  ret = reachable(result, marker) && !marker.frozen?
                  stored = origin != "receiver" && origin != "element" && reachable(recv, marker)
                  yld = yielded.any? { |v| reachable(v, marker) }
                  mutated = snapshot(marker) != before[origin]
                  # Verify retained identity again after changing its state,
                  # through a fresh read of the container, not the old result.
                  if stored
                    begin
                      marker.is_a?(String) ? marker << "_after" : marker.instance_variable_set(:@share_probe, true)
                      stored &&= reachable(recv, marker)
                    rescue FrozenError
                      stored = false
                    end
                  end
                  [ret, stored, yld, mutated].each_with_index do |yes, channel|
                    next unless yes
                    effects[channel] |= bit
                    entry["effects"][channel] |= bit
                    key = "#{%w[return store yield mutate][channel]}:#{origin}"
                    witnesses[key] ||= "#{mode} shape=#{si} arg=#{position} receiver=#{variant} block=#{block}"
                  end
                end
              rescue Exception => e
                errors[e.class.name] += 1
              end
            end
          end
        end
      end
    end
    {"opaque_reads" => @opaque_reads.uniq.sort, "cases" => cases.values.select { |c| c["ok"] > 0 }, "ok" => ok, "attempts" => attempted, "errors" => errors.sort.to_h,
     "effects" => effects, "witnesses" => witnesses.sort.to_h,
     "receiver_return" => receiver_return, "receiver_mutated" => receiver_mutated}
  end

  def self.targets
    require "ostruct"
    source = File.read(File.join(ROOT, "src/builtin_ops.c"))
    hand = source[/static const BopShareRow bop_share_rows\[\] = \{(.*?)\n\};/m]
      .scan(/\{\s*(\w+),\s*"([^"]+)",\s*(BSH_\w+)/)
    targets = []
    (INSTANCE_RECEIVERS.merge("OpenStruct" => -> { OpenStruct.new(a: 1) },
      "StringRange" => -> { "a".."b" }, "BigInteger" => -> { 1 << 70 }, "FloatRange" => -> { 1.0..2.0 }, "FalseClass" => -> { false })).each do |cls, factory|
      fam = FAMILIES[cls]
      names = factory.call.public_methods.map(&:to_s) - PROBE_ARTEFACTS.fetch(cls, [])
      names |= hand.select { |f, _, _| f == fam }.map { |_, n, _| n }.reject { |n| n == "*" }
      names.sort.each { |n| targets << [cls, n, fam, factory] }
    end
    CLASS_TARGETS.merge("Enumerator" => %w[new], "OpenStruct" => %w[new], "ENV" => ENV.public_methods.map(&:to_s),
                        "Kernel" => (CLASS_TARGETS["Kernel"] + hand.select { |f, _, _| f == "BOP_KERNEL" }.map { |_, n, _| n })).each do |cls, names|
      factory = case cls
      when "StructClass" then -> { Struct.new(:a) }
      when "DataClass" then -> { Data.define(:a) }
      else -> { Object.const_get(cls) }
      end
      fam = {"File" => "BOP_FILE_CLASS", "ENV" => "BOP_ENV", "Kernel" => "BOP_KERNEL",
             "Dir" => "TY_CLASS", "Process" => "TY_CLASS"}[cls]
      names.sort.uniq.each do |n|
        f = n == "new" && %w[Array Hash Enumerator OpenStruct].include?(cls) ? "BOP_CLASS_NEW" : fam
        targets << ["#{cls}.", n, f, factory]
      end
    end
    targets
  end

  def self.run
    opts = {}
    OptionParser.new do |p|
      p.on("--write") { opts[:write] = true }
      p.on("--check") { opts[:check] = true }
      p.on("-o FILE") { |v| opts[:out] = v }
    end.parse!
    abort "use CRuby 4.0 with --enable-frozen-string-literal" unless RUBY_ENGINE == "ruby" && RUBY_VERSION.start_with?("4.0.") && "".frozen?
    rows = []
    Dir.mktmpdir("builtin-share") do |dir|
      Dir.chdir(dir) do
        targets.each do |label, name, fam, factory|
          cls = label.delete_suffix(".")
          row = {"class" => label, "method" => name, "family" => fam}
          unsafe = UNSAFE.include?(name) && !(name == "delete" && SAFE_DELETE.include?(cls))
          unsafe ||= %w[Thread Fiber ConditionVariable Mutex].include?(cls)
          if !label.end_with?(".") && FORWARDING.fetch(cls, []).include?(name)
            row["skip"] = "target-dependent forwarding method"
          elsif unsafe
            row["skip"] = "process, filesystem, or blocking operation"
          else
            read, write = IO.pipe
            pid = fork do
              read.close
              $stdin.reopen(File::NULL, "r")
              $stdout.reopen(File::NULL, "w")
              $stderr.reopen(File::NULL, "w")
              # No shared process state survives the child. Files are private.
              File.write("f", "ab\n")
              File.write("a", "ab\n")
              ENV.clear
              ENV["SPINEL_SHARE_ORACLE_SEED"] = "a"
              begin
                write.write(Marshal.dump(probe(label, name, factory)))
              rescue Exception => e
                write.write(Marshal.dump({"skip" => e.class.name}))
              end
              write.close
              exit! 0
            end
            write.close
            begin
              payload = Timeout.timeout(3) { read.read }
              Process.wait(pid)
              row.merge!(payload.empty? ? {"skip" => "child exited without observations"} : Marshal.load(payload))
            rescue Timeout::Error
              Process.kill("KILL", pid) rescue nil
              Process.wait(pid) rescue nil
              row["skip"] = "probe exceeded 3 seconds"
            ensure
              read.close
            end
          end
          rows << row
          $stderr.puts "share oracle: #{rows.size} surfaces (#{label}#{name})" if rows.size % 250 == 0
        end
      end
    end
    data = {"ruby" => "CRuby 4.0", "origins" => BITS, "rows" => rows}
    json = "{\n  \"ruby\": #{data['ruby'].to_json},\n  \"origins\": #{BITS.to_json},\n  \"rows\": [\n" +
      rows.map { |r| "    " + r.to_json }.join(",\n") + "\n  ]\n}\n"
    inc = +"/* Generated by tools/gen_builtin_share_spec.rb from CRuby 4.0: do not edit.\n   Observed return/store/yield/mutation origins: receiver=1, element=2,\n   arg0=4, arg1=8, arg2=16, block=32. No successful call means unknown. */\n"
    rows.each do |r|
      next unless r["family"] && r.fetch("ok", 0) > 0
      r["cases"].each do |c|
        inc << "BSS(#{r['family']},#{r['class'].dump},#{(r["family"] == "BOP_CLASS_NEW" ? r["class"].delete_suffix(".") : r["method"]).dump},#{c['argc']},#{c['block'] ? 1 : 0},#{c['marker'] == 'String' ? 0 : 1},#{c['origins']},#{c['wrapped']},#{c['effects'].join(',')})\n"
      end
    end
    # These exact names replace the scalar/String/IO wildcards. Inherited
    # universal methods keep the existing any-receiver contract. A name is
    # emitted only when every successful marker shape kept no mutable marker.
    hand = File.read(File.join(ROOT, "src/builtin_ops.c"))
      .scan(/\{\s*(\w+),\s*"([^"]+)",\s*BSH_\w+/).map { |fam, name| [fam, name] }
    universal = File.read(File.join(ROOT, "src/builtin_ops.c"))
      .scan(/\{\s*BOP_ANY_RECV,\s*"([^"]+)"/).flatten
    families = %w[TY_STRING TY_IO TY_INT TY_BIGINT TY_FLOAT TY_SYMBOL TY_BOOL TY_NIL
                  TY_RANGE TY_FLOAT_RANGE TY_TIME TY_COMPLEX TY_RATIONAL TY_REGEX TY_MATCHDATA TY_RANDOM]
    pure = "/* Generated by tools/gen_builtin_share_spec.rb from CRuby 4.0: do not edit.\n   Observed keeps-nothing names; see tools/builtin_share_spec.json. */\n"
    rows.group_by { |r| [r["family"], r["method"]] }.sort_by { |k, _| k.map(&:to_s) }.each do |(fam, name), observations|
      next unless families.include?(fam) && !universal.include?(name) && !hand.include?([fam, name])
      next unless observations.all? { |r| r.fetch("ok", 0) > 0 && r["opaque_reads"].empty? && r["effects"][0, 3].all?(&:zero?) && r["effects"][3] & 28 == 0 }
      pure += "  { #{fam}, #{name.dump}, BSH_PURE, 1 },\n"
    end
    table = +"class\tmethod\treturn\tstore\tyield\tmutate\tsuccesses\tstatus\n"
    rows.each do |r|
      fx = (r["effects"] || [0, 0, 0, 0]).map { |mask| BITS.select { |_, b| mask & b != 0 }.keys.join(",") }
      table << [r["class"], r["method"], *fx, r.fetch("ok", 0), r["skip"] || (r["ok"].zero? ? "unprobed: no successful shape" : "observed")].join("\t") + "\n"
    end
    File.write(opts[:out], table) if opts[:out]
    if opts[:write]
      File.write(SPEC, inc)
      File.write(DATA, json)
      File.write(PURE, pure)
    elsif opts[:check]
      unless File.read(SPEC) == inc && File.read(DATA) == json && File.read(PURE) == pure
        old = JSON.parse(File.read(DATA))["rows"].to_h { |r| [[r["class"], r["method"]], r] }
        rows.each do |r|
          prior = old.delete([r["class"], r["method"]])
          next if prior == r
          fields = r.keys.select { |k| !prior || prior[k] != r[k] }
          $stderr.puts "drift: #{r['class']} #{r['method']}: #{fields.join(', ')}"
          fields.each { |k| $stderr.puts "  #{k}: #{prior && prior[k]} -> #{r[k]}" }
        end
        old.each_key { |k| $stderr.puts "drift: #{k.join(' ')}: no longer generated" }
        { SPEC => inc, PURE => pure }.each do |path, text|
          have = File.readlines(path)
          want = text.lines
          (have - want).each { |l| $stderr.puts "drift: #{File.basename(path)}: committed only: #{l.strip}" }
          (want - have).each { |l| $stderr.puts "drift: #{File.basename(path)}: generated only: #{l.strip}" }
        end
        abort "share observations drifted; regenerate with --write"
      end
    else
      puts table unless opts[:out]
    end
    $stderr.puts "share oracle: #{rows.size} surfaces; #{rows.count { |r| r.fetch('ok', 0) > 0 }} observed; #{rows.count { |r| r.fetch('ok', 0).zero? }} unprobed"
  end
end
BuiltinShareProbe.run if $PROGRAM_NAME == __FILE__
