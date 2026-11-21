# Holds spinel's load record (--dump-load-record, src/analyze_elab.c)
# against what CRuby does when it runs the same program's load phase.
#
#   ruby tools/load_record_oracle.rb [--spinel BIN] [--inline] [--timeout S] PROGRAM...
#
# CRuby runs each program (its output thrown away) under a tracer that logs
# what the load phase defines, as the program's own hooks would see it: a
# class or module body opened (TracePoint :class), a method added, removed
# or undefined (method_added, singleton_method_added ...), a constant set
# (const_added), a module included, extended or prepended (included,
# extended, prepended). The hooks are prepended to Module and BasicObject
# and call super; a program's own hook that does not is caught by
# TracePoint :call. An event is placed at the innermost frame of the
# program's own files; one whose frame is a method body, or a block in one,
# runs later than the load phase (the record has no point there) and is
# left out, and so is one on an object other than a module or main, and
# one a C method other than a definition form makes on its own (the
# accessors of Struct.new). Kernel counts as the Object spinel folds it into.
#
# The record gives the same events: each history entry (def, reader, alias,
# undef, const write, mixin, class body ...) at its statement's file and
# line. Visibility words fire no hook, so they are not compared, and nor are
# the names the compiler makes (`__x`, a desugared def's `#__cond1`) or the
# entries of spinel's own builtins/ and packages/ files.
#
# Reported, one line each:
#   cruby-only  an event CRuby ran that the record does not have (one at a
#               point the record holds as opaque for its class is not);
#   spinel-only a sure entry of the record CRuby did not run (an entry that
#               may not run, or one of a form that runs the program's code,
#               is allowed to be missing; nothing is, when CRuby stopped
#               early on an error or the timeout);
#   order       two events CRuby ran in one order and the record holds in
#               the other.
# Exits 1 when any program differs or spinel fails on one (any exit but a
# refusal of --load-model=graph's); a program graph refuses is skipped, and
# so is one CRuby stopped early on with no difference in what it ran, which
# is said. Under graph (the default) each file has its own lines; --inline
# is for a program of one file.
require "rbconfig"
require "tempfile"
require "timeout"

TRACER = <<~'RUBY'
  module LoadRecordOracle
    OUT = File.open(ENV.fetch("LRO_OUT"), "w").tap { |f| f.sync = true }
    SELF = [__FILE__, File.realpath(__FILE__)]
    SYS = [RbConfig::CONFIG["rubylibprefix"], RbConfig::CONFIG["prefix"]].compact.map { |d| File.realpath(d) rescue d }
    LOAD = /\A(<main>|<top \(required\)>|<class:.*>|<module:.*>|singleton class|BEGIN)\z/
    @hook = nil
    # a module by its leaf name ("?" while it has none), Kernel as the Object
    # spinel folds it into, the main object as main, any other object as "-"
    def self.leaf(m)
      return m.equal?(TOPLEVEL_BINDING.receiver) ? "main" : "-" unless m.is_a?(Module)
      return leaf(m.attached_object) if m.singleton_class?
      n = m.name&.split("::")&.last || "?"
      n == "Kernel" ? "Object" : n
    end
    def self.mine?(path)
      !(path.nil? || SELF.include?(path) || path.start_with?("<", "(eval") || SYS.any? { |d| path.start_with?(d) })
    end
    # The innermost frame of the program's own files, after `skip` of them
    # (a hook of the program's own, which runs where it is defined). A C
    # method's frame (attr_reader, Struct.new) carries its caller's file and
    # line, so a frame at the same place as the one below it is passed over:
    # `via` names the innermost one passed over.
    def self.site(skip = 0)
      locs = caller_locations.select { |l| mine?(l.absolute_path || l.path) }.drop(skip)
      i = (0...locs.size - 1).find { |k| [locs[k].path, locs[k].lineno] != [locs[k + 1].path, locs[k + 1].lineno] } || locs.size - 1
      loc = locs[i] or return
      label = loc.label.to_s.sub(/\Ablock (\(\d+ levels\) )?in /, "")
      [loc.absolute_path || loc.path, loc.lineno, LOAD.match?(label) ? "load" : "late", i > 0 ? locs[0].label : ""]
    end
    # a mixin into a singleton class is an extend of its object
    def self.log(kind, owner, name, s = site, hook: false)
      return unless s
      if owner.is_a?(Module) && owner.singleton_class?
        kind = { "include" => "extend", "add" => "sadd", "remove" => "sremove", "undef" => "sundef" }.fetch(kind, kind)
        owner = owner.attached_object
      end
      rec = [*s, kind, leaf(owner), name.to_s]
      # the hook's super reaching Mod logs the event the hook already did
      OUT.puts(rec.join("\t")) unless !hook && rec == @hook
      @hook = hook ? rec : nil
    end
    module Mod
      def method_added(m) = (LoadRecordOracle.log("add", self, m); super)
      def method_removed(m) = (LoadRecordOracle.log("remove", self, m); super)
      def method_undefined(m) = (LoadRecordOracle.log("undef", self, m); super)
      def const_added(n) = (LoadRecordOracle.log("const", self, n); super)
      def included(base) = (LoadRecordOracle.log("include", base, LoadRecordOracle.leaf(self)); super)
      def extended(obj) = (LoadRecordOracle.log("extend", obj, LoadRecordOracle.leaf(self)); super)
      def prepended(base) = (LoadRecordOracle.log("prepend", base, LoadRecordOracle.leaf(self)); super)
    end
    module Obj
      def singleton_method_added(m) = (LoadRecordOracle.log("sadd", self, m); super)
      def singleton_method_removed(m) = (LoadRecordOracle.log("sremove", self, m); super)
      def singleton_method_undefined(m) = (LoadRecordOracle.log("sundef", self, m); super)
    end
    HOOKS = { method_added: "add", method_removed: "remove", method_undefined: "undef", const_added: "const",
              singleton_method_added: "sadd", singleton_method_removed: "sremove",
              singleton_method_undefined: "sundef", included: "include", extended: "extend", prepended: "prepend" }
    # a hook of the program's that does not call super (callee_id: it may
    # be an alias of the program's method)
    TracePoint.new(:call) do |tp|
      kind = HOOKS[tp.callee_id] or next
      next if tp.defined_class == Mod || tp.defined_class == Obj
      arg = tp.binding.local_variable_get(tp.parameters.first&.last) rescue next
      if %w[include extend prepend].include?(kind) then log(kind, arg, leaf(tp.self), site(1), hook: true)
      else log(kind, tp.self, arg, site(1), hook: true)
      end
    end.enable
    TracePoint.new(:class) do |tp|
      path = File.realpath(tp.path) rescue tp.path
      log("open", tp.self, "", [path, tp.lineno, "load", ""]) if mine?(path) && !tp.self.singleton_class?
    end.enable
  end
  Module.prepend(LoadRecordOracle::Mod)
  BasicObject.prepend(LoadRecordOracle::Obj)
RUBY

# The C methods whose effects the record holds; what another C method does
# on its own (Struct.new's accessors and `new`) is not the program's.
FORMS = %w[attr_reader attr_writer attr_accessor attr define_method alias_method remove_method undef_method
           include extend prepend const_set module_function private public protected private_class_method
           public_class_method send public_send __send__ class_eval module_eval class_exec module_exec
           instance_eval instance_exec define_singleton_method singleton_class].freeze

# CRuby's events of the load phase, [path, line, kind, owner, name], and
# whether the program ran to its end
def cruby_events(prog, timeout)
  prog = File.expand_path(prog)
  Tempfile.create(["lro_tracer", ".rb"]) do |tr|
    Tempfile.create("lro_out") do |out|
      tr.write(TRACER); tr.flush
      pid = Process.spawn({ "LRO_OUT" => out.path }, RbConfig.ruby, "--enable-frozen-string-literal", "-r", tr.path, prog,
                          in: File::NULL, out: File::NULL, err: File::NULL, chdir: File.dirname(prog))
      ok = begin
        Timeout.timeout(timeout) { Process.wait(pid) }
        $?.success?
      rescue Timeout::Error
        Process.kill("KILL", pid); Process.wait(pid)
        false
      end
      # as bytes, like spinel's record, so a name past ASCII compares equal
      ev = File.readlines(out.path, chomp: true, mode: "rb").map { |l| l.split("\t", -1) }
               .select { |e| e[2] == "load" && e[5] != "-" && !(e[3] =~ /\A[\w:]+[#.](\S+)\z/ && !FORMS.include?($1)) }
               .map { |p, l, _, _, *rest| [p, l.to_i, *rest] }
      [ev, ok]
    end
  end
end

# spinel's output as bytes: a name or path in it need not be valid in the
# locale's encoding (under LANG=C, any byte past ASCII), and the regexps
# below would raise on it
def run(*cmd)
  out = IO.popen(cmd, "rb", err: [:child, :out], &:read)
  [$?.success?, out]
end

# spinel's names as CRuby spells them: a class by its leaf (a path, or the
# Outer__Leaf a colliding name is renamed to), main as Object, a method
# without the marks a desugared version carries
def leaf(n)
  return "Object" if n == "main"
  return "?" if n.nil? || n == "?" || n.include?("SpinelAnon")
  n = "Class" if n == "Class__reopen"
  n.split(/::|__(?=[A-Z])/).last
end

def meth(n) = n.sub(/(#__cond\d+|#redef\d+|__redef\d+|__main\d+)\z/, "")

def hidden?(name) = name.start_with?("__") || name.include?("#") || name.include?("SpinelAnon")

# The record's events: [path, line, kind, owner, name, seq, may]; or nil,
# what spinel said, and whether that is a refusal of the graph model
def spinel_events(spinel, prog, inline)
  mode = [inline ? "--load-model=inline" : "--load-model=graph"]
  ok, rec = run(spinel, *mode, "--dump-load-record", prog)
  unless ok
    refusal = rec.lines.map(&:strip).find { |l| l.match?(/:\d+: --load-model=graph\b/) }
    return [nil, refusal, true] if refusal
    return [nil, rec.lines.last.to_s.strip.then { |l| l.empty? ? $?.to_s : l }, false]
  end
  files = {}
  if inline
    files[0] = File.expand_path(prog).b
  else
    ok, graph = run(spinel, "--load-model=graph", "--dump-load-graph", prog)
    # without the graph no entry has a file, and every one would be dropped
    return [nil, graph.lines.last.to_s.strip.then { |l| l.empty? ? $?.to_s : l }, false] unless ok
    graph.scan(/^file (\d+) (.*)$/) { files[$1.to_i] = $2 }
  end
  own = %w[builtins packages].map { |d| File.join(File.dirname(File.expand_path(spinel)), d, "").b }
  st = {}
  ev = []
  opaque = []
  forms = rec.scan(/^form #(\d+) runs code/).flatten.map(&:to_i)
  rec.each_line(chomp: true) do |l|
    if (m = l.match(/\A\s*r-?\d+ #(\d+)( may)? f(\d+) (\w+)\S*(?: \S+)? line (\d+)\z/))
      st[m[1].to_i] = { may: !!m[2], path: files[m[3].to_i], kind: m[4], line: m[5].to_i }
      next
    end
    add = lambda do |kind, owner, name, seq, may|
      s = st[seq] or return
      next if s[:path].nil? || s[:path].start_with?(*own)
      ev << [s[:path], s[:line], kind, owner, name, seq, may || s[:may] || forms.include?(seq)]
    end
    if (m = l.match(/\Aopaque (\S+): (.*)\z/))
      m[2].scan(/opaque#(\d+)/) { |seq,| (s = st[seq.to_i]) && opaque << [s[:path], s[:line], leaf(m[1])] }
    elsif (m = l.match(/\Aclass (\S+): (.*)\z/))
      m[2].scan(/(body|include|extend|prepend)(?: (\S+))?#(\d+)/) do |k, mod, seq|
        seq = seq.to_i
        if k == "body" then add.("open", leaf(m[1]), "", seq, false) if %w[ClassNode ModuleNode].include?(st.dig(seq, :kind))
        else add.(k, m[1] == "main" ? "main" : leaf(m[1]), leaf(mod), seq, false)
        end
      end
    elsif (m = l.match(/\Aconst (?:(\S+?)::)?([^:\s]+): (.*)\z/))
      next if hidden?(m[2])
      m[3].scan(/write#(\d+)( may)?/) { |seq, may| add.("const", leaf(m[1] || "main"), m[2], seq.to_i, !!may) }
    elsif (m = l.match(/\A(\S+?)([#.])(\S+): (.*)\z/)) && !l.start_with?("$", "main @", "visibility", "opaque", "form ")
      name = meth(m[3])
      next if hidden?(name)
      body = m[4].gsub(/\([^)]*\)/, "")
      body.scan(/(\w+)#(\d+)( may)?/) do |k, seq, may|
        kind = case k
               when "def", "define_method", "reader", "writer", "alias", "copy" then m[2] == "." ? "sadd" : "add"
               when "undef", "remove" then m[2] == "." ? "s#{k}" : k
               end
        add.(kind, leaf(m[1]), name, seq.to_i, !!may) if kind
      end
    end
  end
  [ev, opaque]
end

# "?" (a module with no name yet, or one the record does not name) matches any
def like?(a, b) = a == b || a == "?" || b == "?"

def same?(c, s) = c[0, 3] == s[0, 3] && like?(c[3], s[3]) && like?(c[4], s[4])

# An event CRuby ran at a point the record holds as opaque for its owner (a
# define_method or send whose name is not a literal) is not missing. When
# CRuby stopped early (an error, a timeout), what the record has past that
# is not compared.
def compare(cruby, spinel, opaque, finished)
  out = []
  used = {}
  matched = []
  cruby.each do |c|
    next if c[2] == "add" && c[4].start_with?("__") || c[2] == "sadd" && c[4].start_with?("__")
    i = spinel.each_index.find { |k| !used[k] && same?(c, spinel[k]) }
    if i then used[i] = true; matched << [c, spinel[i][5]]
    elsif !opaque.any? { |o| o[0, 2] == c[0, 2] && like?(o[2], c[3]) }
      out << "#{c[0]}:#{c[1]} cruby-only #{c[2]} #{c[3]} #{c[4]}".rstrip
    end
  end
  spinel.each_with_index do |s, k|
    next if used[k] || s[6] || !finished
    out << "#{s[0]}:#{s[1]} spinel-only #{s[2]} #{s[3]} #{s[4]}".rstrip
  end
  matched.each_cons(2) do |(a, sa), (b, sb)|
    out << "#{b[0]}:#{b[1]} order #{b[2]} #{b[3]} #{b[4]} recorded before #{a[2]} #{a[3]} #{a[4]} (#{a[0]}:#{a[1]})" if sb < sa
  end
  out
end

spinel = File.expand_path("../spinel", __dir__)
inline = false
timeout = 30
progs = []
args = ARGV.dup
while (a = args.shift)
  case a
  when "--spinel" then spinel = args.shift
  when "--inline" then inline = true
  when "--timeout" then timeout = args.shift.to_f
  else progs << File.expand_path(a)
  end
end
abort "usage: ruby tools/load_record_oracle.rb [--spinel BIN] [--inline] [--timeout S] PROGRAM..." if progs.empty?

agree = differ = 0
progs.each do |prog|
  sev, opaque, refused = spinel_events(spinel, prog, inline)
  if sev.nil?
    puts refused ? "#{prog}: skipped, spinel: #{opaque}" : "#{prog}: spinel failed: #{opaque}"
    differ += 1 unless refused
    next
  end
  cev, finished = cruby_events(prog, timeout)
  diffs = compare(cev, sev, opaque, finished)
  puts "#{prog}: CRuby stopped early, its later events not compared" unless finished
  diffs.each { |d| puts d }
  if !diffs.empty? then differ += 1
  elsif finished then agree += 1
  end
end
puts "load record oracle: #{agree} agree, #{differ} differ, #{progs.size - agree - differ} skipped" if progs.size > 1
exit(differ.zero? ? 0 : 1)
