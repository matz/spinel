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
{ "parentheses" => "(value)", "to_s" => "value.to_s",
  "ternary" => "value.is_a?(String) ? value : nil" }.each do |shape, expression|
  cases["super_expression_#{shape}"] = ["Child.new.store(t); nil", "", "abc!", <<~RUBY]
    class Parent
      def store(value)
        if value.is_a?(String); items = [value]; items[0] << '!'; end
        nil
      end
    end
    class Child < Parent
      def store(value) = super(#{expression})
    end
    Child.new.store(0)
  RUBY
end
cases["super_block_escape"] = ["Child.new.store(t); nil", "", "abc!", <<~RUBY]
  class Parent
    def store(unused)
      items = yield
      items[0] << '!' if items[0].is_a?(String)
      nil
    end
  end
  class Child < Parent
    def store(value) = super(0) { [value] }
  end
  Child.new.store(0)
RUBY
%w[each_byte each_char each_line].each do |method|
  cases["iterator_return_#{method}"] = ["t.#{method} { |element| element }", "result << '!'", "abc!"]
end
cases["readonly_iterator"] = ["t.each_byte { |element| element }; nil", "", "abc"]
cases["root_poly_actual"] = ["if t.is_a?(String); items = [t]; items[0] << '!'; end; nil", "", "abc!", <<~RUBY]
  class Reader
    def root_poly_actual(value) = one(value)
  end
RUBY
cases["root_poly_readonly"] = ["t.is_a?(String) ? t.bytesize : 0", "", "abc", <<~RUBY]
  class Reader
    def root_poly_readonly(value) = one(value)
  end
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
cases["root_direct_box_store"] = ["nil", "", "abc!", <<~RUBY]
  class Reader
    def direct_box_reader(value) = value.is_a?(String) ? value.bytesize : 0
    def root_direct_box_store(value)
      items = []
      items.push(value)
      items[0] << '!' if value.is_a?(String)
      direct_box_reader(value)
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
[4, 5].each do |levels|
  methods = (0...levels).map do |i|
    left = i == levels - 1 ? "LeftDocument.new" : "level#{i + 1}(flag)"
    right = i == levels - 1 ? "RightDocument.new" : "level#{i + 1}(flag)"
    "def self.level#{i}(flag); return #{left} if flag; #{right}; end"
  end.join("\n")
  name = levels == 4 ? "readonly_family_diamond" : "return_family_budget"
  cases[name] = ["document.parse(t); nil", "", "abc", family + <<~RUBY]
    class Factory
      def self.wrap(flag) = level0(flag)
      #{methods}
    end
  RUBY
end

# Compilation is an explicit requirement for these controls, not a property
# inferred from their names. Renaming/removing one without updating this list
# fails immediately; every other case must produce the identity refusal.
native_controls = %w[
  preserved_rest preserved_yield preserved_append_and_store
  readonly readonly_iterator readonly_guard_alias readonly_array_search readonly_keyword
  readonly_keyword_only readonly_keyword_post_rest readonly_predicate
  preserved_cached_mutator readonly_return_family readonly_family_diamond root_poly_readonly
]
native_controls.each { |name| cases.fetch(name) }

failures = []
refusals = 0
native_passes = 0
Dir.mktmpdir("spinel-forward-escapes") do |dir|
  cases.each do |name, (body, followup, want, prefix, guard, before)|
    source = File.join(dir, "#{name}.rb")
    cfile = File.join(dir, "#{name}.c")
    keyword_entry = name.end_with?("keyword_only", "keyword_post_rest")
    entry = keyword_entry || name == "keyword_root" || name.start_with?("root_") ? name : "coerce"
    seed = entry == "coerce" ? "nil" : "1"
    actual = "s"
    actual = "[s, 0][ARGV.length]" if name.start_with?("root_poly_")
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
      native_passes += 1
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("through a parameter it hands on escapes") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "#{name}: not an identity refusal (status #{status.exitstatus}): #{out}#{err}"
    end
    refusals += 1
  end
  # Shared-field writes need an alias proof even when their result is
  # discarded. Do not add a per-route sharing exception for these stores.
  ["other.notice = @notice", "return other.notice = @notice", "other.notice = @notice; nil"].each_with_index do |tail, i|
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
      #{i == 2 ? "other.notice" : "returned"} << '#'
      p first.notice, other.notice, returned
    RUBY
    out, err, status = Open3.capture3(RbConfig.ruby, source)
    expected = "\"notice!#\"\n" * 2 + (i == 2 ? "nil\n" : "\"notice!#\"\n")
    unless status.success? && out == expected
      failures << "writer_return_#{i}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("an attribute writer given a String") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "writer_return_#{i}: unsafe writer admitted: #{out}#{err}"
    end
    refusals += 1
  end
  # Preserving an incoming box is conditional, not a readonly suffix.
  # Mutable scalar/array inputs and replaced or aliased array elements
  # must still refuse, including when a rest forwards to that same store.
  boxed_inputs = {
    "scalar" => "s = +'abc'; collector.accept(s)",
    "array" => "s = +'abc'; collector.gather([s])",
    "replaced" => "s = +'abc'; values = ['old']; values[0] = s; collector.gather(values)",
    "aliased" => "s = +'abc'; values = ['old']; alias_values = values; alias_values[0] = s; collector.gather(values)",
    "rest" => "s = +'abc'; collector.rest(s)",
    "case_conversion_local" => "s = (+'ABC').downcase; collector.accept(s)",
    "case_conversion_temporary" => "collector.accept((+'AbC').downcase); s = collector.at(1)",
    "dynamic_constructor" => <<~RUBY,
      class Box
        def initialize(values)
          @items = []
          hold(7)
          values.each { |value| hold(value) }
        end
        def hold(value); @items.push(value); nil; end
        def at(index) = @items[index]
      end
      Box.new(['seed'])
      s = +'abc'
      klass = [Box][0]
      collector = klass.new([s])
    RUBY
    "outgoing_super" => <<~RUBY,
      $source = s = +'abc'
      class Collector
        def gather(values); values[0] = $source; nil; end
      end
      class DerivedCollector < Collector
        def gather(values); super; values.to_a.each { |value| store(value) }; nil; end
      end
      collector = DerivedCollector.new
      collector.accept(7)
      collector.gather(['old'])
    RUBY
    "helper_super" => <<~RUBY,
      $source = s = +'abc'
      class Collector
        def check(values); values[0] = $source; nil; end
      end
      class DerivedCollector < Collector
        def check(values); super; nil; end
        def gather(values); check(values); values.to_a.each { |value| store(value) }; nil; end
      end
      collector = DerivedCollector.new
      collector.accept(7)
      collector.gather(['old'])
    RUBY
    "helper_override" => <<~RUBY
      $source = s = +'abc'
      class Collector
        def check(values) = values.length
        def gather(values); check(values); values.to_a.each { |value| store(value) }; nil; end
      end
      class DerivedCollector < Collector
        def check(values); values[0] = $source; nil; end
      end
      collector = DerivedCollector.new
      collector.accept(7)
      collector.gather(['old'])
    RUBY
  }
  { "comparison" => ["<=>", ""], "comparison_alias" => ["compare", "alias <=> compare"],
    "index" => ["[]=", ""], "index_alias" => ["put", "alias []= put"] }.each do |name, (method, alias_decl)|
    comparison = name.start_with?("comparison")
    boxed_inputs[name] = <<~RUBY
      class Box
        include Comparable
        def initialize = @items = []
        def hold(value); @items.push(value); nil; end
        def #{method}(#{comparison ? "value" : "key, value"})
          hold(value) if value.is_a?(String)
          #{comparison ? "0" : "nil"}
        end
        #{alias_decl}
        def [](key) = nil
        def at(index) = @items.last
      end
      box = Box.new
      box.hold(7)
      #{comparison ? "box <=> 'seed'" : "box[0] = 'seed'"}
      s = +'abc'
      #{comparison ? "[box, s].sort" : "candidate = [box, 0][0]; candidate[0] ||= s"}
      collector = box
    RUBY
  end
  { "coerce_modulo" => "%", "coerce_nonunique_plus" => "+" }.each do |name, operator|
    boxed_inputs[name] = <<~RUBY
      class Box
        def initialize = @items = []
        def hold(value); @items.push(value); nil; end
        def #{operator}(value); hold(value) if value.is_a?(String); 0; end
        def at(index) = @items.last
      end
      class OtherBox
        def +(value) = 0
      end
      class Proxy
        def coerce(value) = [$target, $source]
      end
      box = $target = Box.new
      box.hold(7)
      box.#{operator}(0)
      box.#{operator}('seed')
      s = $source = +'abc'
      #{operator == '%' ? '1.modulo(Proxy.new)' : '1 + Proxy.new'}
      collector = box
    RUBY
  end
  %w[upcase downcase capitalize swapcase].each do |method|
    boxed_inputs["case_conversion_override_#{method}"] = <<~RUBY
      class String
        def #{method} = self
      end
      s = +'abc'
      collector.accept(s.#{method})
    RUBY
  end
  { "shift_fold" => "[s].reduce(box, :<<)", "shift_write" => "box <<= s" }.each do |name, call|
    [false, true].each do |aliased|
      boxed_inputs["#{name}#{aliased ? '_alias' : ''}"] = <<~RUBY
        class Box
          def initialize = @items = []
          def hold(value); @items.push(value); nil; end
          def #{aliased ? 'take' : '<<'}(value); hold(value); self; end
          #{aliased ? 'alias << take' : ''}
          def at(index) = @items.last
        end
        box = Box.new
        box.hold(7)
        box << 'seed'
        s = +'abc'
        #{call}
        collector = box
      RUBY
    end
  end
  boxed_inputs["dynamic_constructor_alias"] = <<~RUBY
    class Box
      def setup(values)
        @items = []
        hold(7)
        values.each { |value| hold(value) }
      end
      alias initialize setup
      def hold(value); @items.push(value); nil; end
      def at(index) = @items[index]
    end
    Box.new(['seed'])
    s = +'abc'
    klass = [Box][0]
    collector = klass.new([s])
  RUBY
  boxed_inputs.each do |name, input|
    source = File.join(dir, "boxed_#{name}.rb")
    cfile = File.join(dir, "boxed_#{name}.c")
    File.write(source, <<~RUBY)
      class Collector
        def initialize = @items = []
        def accept(value); store(value); nil; end
        def rest(*values); store(*values); nil; end
        def store(value); @items.push(value); nil; end
        def gather(values); values.to_a.each { |value| store(value) }; nil; end
        def at(index) = @items[index]
      end
      collector = Collector.new
      collector.accept(7)
      #{input}
      collector.at(1) << '!'
      p s
    RUBY
    out, err, status = Open3.capture3(RbConfig.ruby, source)
    unless status.success? && out == "\"abc!\"\n"
      failures << "boxed_#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("through a parameter it hands on escapes") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "boxed_#{name}: unsafe box retention admitted: #{out}#{err}"
    end
    refusals += 1
  end
  # An RBS-seeded Hash must not enter an unrelated shared-String setter arm.
  # Keep valid String and nil calls too: excluding every arm is not a fix.
  source = File.join(dir, "shared_hash_arm.rb")
  signature = File.join(dir, "shared_hash_arm.rbs")
  executable = File.join(dir, "shared_hash_arm")
  File.write(signature, "class StringSink\n  def []=: (Symbol key, String? value) -> void\nend\n")
  File.write(source, <<~RUBY)
    class StringSink
      attr_reader :value
      def initialize = @value = nil
      def []=(key, value)
        @value = value
        value << "!" unless value.nil?
        nil
      end
    end
    def receiver(flag) = flag ? {} : StringSink.new
    hash = receiver(true)
    hash[:payload] = { "nested" => 37 }
    p hash[:payload]["nested"]
    sink = receiver(false)
    text = +"abc"
    alias_text = text
    sink[:value] = text
    p sink.value, text, alias_text
    sink[:value] = nil
    p sink.value
  RUBY
  expected = "37\n" + "\"abc!\"\n" * 3 + "nil\n"
  out, err, status = Open3.capture3(RbConfig.ruby, source)
  if !status.success? || out != expected
    failures << "shared_hash_arm: invalid CRuby reduction: #{out.inspect} #{err}"
  else
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "--rbs", signature, "-o", executable)
    if !status.success?
      failures << "shared_hash_arm: native control refused: #{out}#{err}"
    else
      out, err, status = Open3.capture3(timeout, "30", executable)
      failures << "shared_hash_arm: native control differs: #{out.inspect} #{err}" unless status.success? && out == expected
      native_passes += 1
    end
  end
end
abort failures.join("\n") unless failures.empty?
puts "forward-escape-check: #{refusals} independent CRuby-validated refusals, #{native_passes} native readonly/identity controls pass"
