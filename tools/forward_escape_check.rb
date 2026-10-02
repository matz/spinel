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
cases["readonly_keyword"] = ["keyword_read(value: t)", "", "abc"]
cases["keyword_escape"] = ["keyword_store(value: t)", "", "abc!"]
cases["keyword_root"] = ["nil", "", "abc!"]
cases["keyword_expression"] = ["keyword_store(value: (t.is_a?(String) ? t : nil))", "", "abc!"]
%w[keyword_only keyword_post_rest].each do |shape|
  signature = shape == "keyword_only" ? "value:" : "*unused, last, value:"
  [false, true].each do |readonly|
    name = "#{readonly ? 'readonly' : 'root'}_#{shape}"
    target = readonly ? "keyword_read" : "keyword_store"
    cases[name] = ["nil", "", readonly ? "abc" : "abc!", <<~RUBY]
      class Reader
        def #{name}(#{signature}) = #{target}(value: value)
      end
    RUBY
  end
end
cases["readonly_predicate"] = ["['abc'].any? { |other| other.eql?(t) }", "", "abc"]
cases["predicate_escape"] = ["['abc'].any? { a = [t]; a[0] << '!' }; nil", "", "abc!"]
cases["predicate_lambda"] = ["['abc'].any? { $later = -> { t.bytesize }; false }; nil",
                            "s << '!'; raise 'stale capture' unless $later.call == 4", "abc!"]
cases["override_predicate"] = ["['abc'].any? { t.bytesize }; nil",
                              "s << '!'; raise 'stale capture' unless $later.call == 4", "abc!",
                              "class Array; def any?(&block); $later = block; false; end; end"]
cases["override_predicate_equality"] = ["['abc'].any? { |other| other.eql?(t) }; nil", "$held << '!'", "abc!",
                                       "$held = nil; class String; def eql?(value); $held = value; false; end; end"]
cases["root_literal_store"] = ["nil", "", "abc!", <<~RUBY]
  class Reader
    def root_literal_store(data)
      items = [data]
      items[0] << '!' if data.is_a?(String)
      keyword_read(value: data)
      nil
    end
  end
RUBY
{ "super" => "super", "explicit" => "super(value)" }.each do |shape, call|
  name = "root_store_#{shape}"
  cases[name] = ["nil", "", "abc!", <<~RUBY]
    class BoxParent
      def initialize = @items = []
      def store(value); @items.push(value); nil; end
      def mutate; @items[-1] << '!'; nil; end
    end
    class BoxChild < BoxParent
      def store(value) = #{call}
    end
    class Reader
      def #{name}(data)
        box = BoxChild.new
        box.store(data)
        box.mutate if data.is_a?(String)
        nil
      end
    end
  RUBY
end

# Query a terminal directly before reaching it through a POLY chain. A
# root-only zero must not become a reusable proof for a retained suffix.
warm = "(terminal(+'warm'); data.is_a?(String))"
cases["cached_return"] = ["t", "result << '!'", "abc!", "", warm]
cases["cached_local_retention"] = ["a = t; a = 0; t.bytesize", "", "abc", "", warm]
cases["preserved_cached_mutator"] = ["t << '!'; nil", "", "abc!", "", warm]
# Even a genuinely readonly cached suffix is still a forwarding edge from
# a copied root occurrence; it must not hide that root's unsupported store.
cases["cached_readonly_early_store"] = ["t.bytesize", "", "abc", "", nil,
                                      "keyword_read(value: 'warm'); @snapshot = [t];"]

# A boxed receiver returned through a constructor union has an object-family
# proof, not permission to ignore actual descendant overrides or unknown
# leaves. A foreign parse method must not poison the proven readonly family.
family = <<~RUBY
  class Document
    def parse(t) = t.bytesize
  end
  class LeftDocument < Document; end
  class RightDocument < Document; end
  class ForeignParser
    def parse(t)
      if t.is_a?(String); a = [t]; a[0] << '!'; end
      nil
    end
  end
  ForeignParser.new.parse(nil)
  class Factory
    def self.wrap(flag)
      return LeftDocument.new if flag
      RightDocument.new
    end
  end
  class Reader
    def document = Factory.wrap(false)
  end
RUBY
cases["readonly_return_family"] = ["document.parse(t); nil", "", "abc", family]
cases["return_family_override"] = ["document.parse(t); nil", "", "abc!", family + <<~RUBY]
  class RightDocument
    def parse(t)
      if t.is_a?(String); a = [t]; a[0] << '!'; end
      nil
    end
  end
  RightDocument.new.parse(nil)
RUBY
cases["return_family_unknown"] = ["document.parse(t); nil", "", "abc", family + <<~RUBY]
  class Factory
    def self.wrap(flag)
      options = [RightDocument.new, ForeignParser.new]
      options[0]
    end
  end
RUBY

# Compilation is an explicit requirement for these controls, not a property
# inferred from their names. Renaming/removing one without updating this list
# fails immediately; every other case must produce the identity refusal.
native_controls = %w[
  preserved_rest preserved_yield preserved_append_and_store
  readonly readonly_guard_alias readonly_array_search readonly_keyword
  readonly_keyword_only readonly_keyword_post_rest readonly_predicate
  preserved_cached_mutator readonly_return_family
]
native_controls.each { |name| cases.fetch(name) }

failures = []
Dir.mktmpdir("spinel-forward-escapes") do |dir|
  cases.each do |name, (body, followup, want, prefix, guard, before)|
    source = File.join(dir, "#{name}.rb")
    cfile = File.join(dir, "#{name}.c")
    keyword_entry = name.end_with?("keyword_only", "keyword_post_rest")
    entry = keyword_entry || name == "keyword_root" || name.start_with?("root_") ? name : "coerce"
    seed = entry == "coerce" ? "nil" : "1"
    actual = "s"
    if keyword_entry
      positional = name.end_with?("keyword_post_rest") ? "99, 7, " : ""
      seed = "#{positional}value: 1"
      actual = "#{positional}value: s"
    end
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
        def keyword_root(data) = keyword_store(value: data)
        def keyword_read(value:) = value.is_a?(String) ? value.bytesize : 0
        def keyword_store(value:)
          if value.is_a?(String); a = [value]; a[0] << '!'; end
          nil
        end
      end
      reader = Reader.new
      reader.#{entry}(#{seed})
      s = +'abc'
      other = s
      result = reader.#{entry}(#{actual})
      #{followup}
      p s, other
    RUBY
    out, err, status = Open3.capture3(RbConfig.ruby, source)
    expected = "#{want.inspect}\n#{want.inspect}\n"
    unless status.success? && out == expected
      failures << "#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    if native_controls.include?(name)
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
  # A discarded writer can share a handle. Returning that assignment needs
  # a separate caller alias proof: do not admit a copied result that mutates
  # independently of the two fields. Test implicit and explicit exits alone.
  ["other.notice = @notice", "return other.notice = @notice"].each_with_index do |tail, i|
    source = File.join(dir, "writer_return_#{i}.rb")
    cfile = File.join(dir, "writer_return_#{i}.c")
    File.write(source, <<~RUBY)
      class Notices
        attr_accessor :notice
        def initialize(text) = @notice = text
        def assign(other)
          #{tail}
        end
      end
      first = Notices.new(+'notice')
      other = Notices.new(nil)
      first.notice << '!'
      returned = first.assign(other)
      returned << '#'
      p first.notice, other.notice, returned
    RUBY
    out, err, status = Open3.capture3(RbConfig.ruby, source)
    unless status.success? && out == "\"notice!#\"\n" * 3
      failures << "writer_return_#{i}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("an attribute writer given a String") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "writer_return_#{i}: unsafe value writer admitted: #{out}#{err}"
    end
  end
end
abort failures.join("\n") unless failures.empty?
puts "forward-escape-check: #{cases.length - native_controls.length + 2} independent CRuby-validated refusals, #{native_controls.length} native readonly/identity controls pass"
