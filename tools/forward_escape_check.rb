# Independent, one-program-per-case controls for #6782. Unsupported escapes
# must identify lost String identity and write no C, not fail arbitrarily.
# Supported box/handle paths must build and match the independently checked
# CRuby caller/alias answers; a refusal is not a pass for those controls.
require "open3"
require "rbconfig"
require "tmpdir"

compiler, timeout = ARGV
abort "usage: ruby tools/forward_escape_check.rb COMPILER TIMEOUT" unless compiler && timeout
compiler = File.expand_path(compiler)
timeout = File.expand_path(timeout)

cases = {
  "array" => ["a = [t]; a[0] << '!'", "", "abc!"],
  "hash" => ["a = { value: t }; a[:value] << '!'", "", "abc!"],
  "ivar" => ["@held = t; @held << '!'", "", "abc!"],
  "global" => ["$held = t; $held << '!'", "", "abc!"],
  "block" => ["[t].each { |u| u << '!' }", "", "abc!"],
  "tap" => ["t.tap { |u| u << '!' }", "", "abc!"],
  "return" => ["t", "result << '!'", "abc!"],
  "alias_return" => ["a = t", "result << '!'", "abc!"],
  "ident" => ["ident(t) << '!'", "", "abc!"],
  "ternary" => ["(t.is_a?(String) ? t : nil) << '!'", "", "abc!"],
  "preserved_rest" => ["rest_store(t)", "", "abc!"],
  "rest_escape" => ["rest_escape(t)", "", "abc!"],
  "via_rest_escape" => ["via_rest(t)", "", "abc!"],
  "early_store" => ["a = [t]; a[0] << '!'", "", "abc!", "", nil, "@snapshot = [t];"],
  "post_rest" => ["post_store(t)", "", "abc!"],
  "preserved_yield" => ["yield t", "", "abc!"],
  "unless_else" => ["unless t.is_a?(String); nil; else; a = [t]; a[0] << '!'; end", "", "abc!"],
  "preserved_append_and_store" => ["t << 'a'; a = [t]; a[0] << 'b'", "", "abcab"],
  "closure" => ["-> { t.bytesize }", "s << '!'; raise 'stale capture' unless result.call == 4", "abc!"],
  "to_s_alias" => ["a = t.to_s; a << '!'", "", "abc!"],
  "parenthesized" => ["ident((t)) << '!'", "", "abc!"]
}

# The same stores with mutators that previously produced bad C in a narrowed
# body. Keeping them in separate programs prevents one rejection hiding another.
{ "assign" => "[0] = '!'", "concat" => ".concat('!')", "insert" => ".insert(3, '!')",
  "prepend" => ".prepend('!')", "setbyte" => ".setbyte(0, 33)",
  "slice" => ".slice!(0)", "chained" => " << '!' << '?'",
  "public_send" => ".public_send(:concat, '!')" }.each do |name, operation|
  want = { "assign" => "!bc", "prepend" => "!abc", "setbyte" => "!bc",
           "slice" => "bc", "chained" => "abc!?" }.fetch(name, "abc!")
  cases[name] = ["a = [t]; a[0]#{operation}", "", want]
end

# An overridden guard/read/identity method must not inherit a builtin proof.
%w[is_a? kind_of?].each do |method|
  cases["override_#{method}"] = [
    "if t.#{method}(String); nil; else; a = [t]; a[0] << '!'; end", "", "abc!",
    "class String; def #{method}(klass) = false; end", "data"
  ]
end
%w[to_s itself].each do |method|
  cases["override_#{method}"] = [
    "a = t.#{method}; if a.is_a?(String); nil; else; a = [t]; a[0] << '!'; end", "", "abc!",
    "class String; def #{method} = 1; end"
  ]
end
cases["override_bytesize"] = ["t.bytesize; nil", "$held << '!'", "abc!",
                               "$held = nil; class String; def bytesize; $held = self; 0; end; end"]
cases["override_array_search"] = ["'abc'.split('\n').include?(t); nil", "$held << '!'", "abc!",
                                  "$held = nil; class Array; def include?(value); $held = value; false; end; end"]
cases["override_element_equality"] = ["['abc'].include?(t); nil", "$held << '!'", "abc!",
                                      "$held = nil; class String; def ==(value); $held = value; false; end; end"]
["super", "super(*r)"].each do |forward|
  cases["post_rest_#{forward}"] = ["Child.new.store(t); nil", "", "abc!", <<~RUBY]
    class Parent
      def store(*unused, last); if last.is_a?(String); a = [last]; a[0] << '!'; end; nil; end
    end
    class Child < Parent
      def store(*r); #{forward}; nil; end
    end
    Child.new.store(0)
  RUBY
end
cases["poly_receiver_rest"] = ["rest_receiver(t, Holder.new); nil", "", "abc!", <<~RUBY]
  class Holder
    def store(value); if value.is_a?(String); a = [value]; a[0] << '!'; end; nil; end
  end
  class Ignorer
    def store(value) = nil
  end
  class Reader
    def rest_receiver(value, receiver); rest_send(receiver, value); nil; end
    def rest_send(receiver, *r); receiver.store(*r); nil; end
  end
  Reader.new.rest_receiver(0, Ignorer.new)
RUBY
cases["readonly"] = ["t.bytesize", "", "abc"]
cases["readonly_guard_alias"] = ["return t if t.is_a?(Other); a = t.to_s; a.bytesize", "", "abc", "class Other; end"]
cases["readonly_array_search"] = ["'abc'.split('\n').include?(t)", "", "abc"]

failures = []
Dir.mktmpdir("spinel-forward-escapes") do |dir|
  cases.each do |name, (body, followup, want, prefix, guard, before)|
    source = File.join(dir, "#{name}.rb")
    cfile = File.join(dir, "#{name}.c")
    File.write(source, <<~RUBY)
      #{prefix}
      class Reader
        def coerce(data)
          return one(data) if #{guard || "data.is_a?(String)"}
          nil
        end
        def one(t); #{before} two(t); end
        def two(t) = three(t)
        def three(t) = four(t)
        def four(t) = five(t)
        def five(t) = six(t)
        def six(t) = terminal(t) { |u| u << '!' }
        def terminal(t)
          #{body}
        end
        def ident(t) = t
        def rest_store(*r); r[0] << '!'; nil; end
        def rest_escape(*r); a = [r[0]]; a[0] << '!'; nil; end
        def via_rest(*r); rest_sink(*r); nil; end
        def rest_sink(t); if t.is_a?(String); a = [t]; a[0] << '!'; end; nil; end
        def post_store(*unused, last); a = [last]; a[0] << '!'; nil; end
      end
      reader = Reader.new
      reader.coerce(nil)
      s = +'abc'
      other = s
      result = reader.coerce(s)
      #{followup}
      p s, other
    RUBY
    out, err, status = Open3.capture3(RbConfig.ruby, source)
    expected = "#{want.inspect}\n#{want.inspect}\n"
    unless status.success? && out == expected
      failures << "#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    if name.start_with?("readonly", "preserved")
      executable = File.join(dir, name)
      out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
      unless status.success?
        failures << "#{name}: native control refused: #{out}#{err}"
        next
      end
      out, err, status = Open3.capture3(timeout, "30", executable)
      failures << "#{name}: native control differs: #{out.inspect} #{err}" unless status.success? && out == expected
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("through a parameter it hands on escapes") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "#{name}: not an identity refusal (status #{status.exitstatus}): #{out}#{err}"
    end
  end
end
abort failures.join("\n") unless failures.empty?
controls = cases.keys.count { |name| name.start_with?("readonly", "preserved") }
puts "forward-escape-check: #{cases.length - controls} independent CRuby-validated refusals, #{controls} native readonly/identity controls pass"
