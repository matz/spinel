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
escape_path = /through (?:a parameter|the rest) it hands on escapes/

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

# Non-plain writes must retain their input even in statement position.
{ "ivar_or_write" => "@held ||= t; @held << '!'",
  "global_or_write" => "$held ||= t; $held << '!'",
  "index_or_write" => "h = {}; h[:k] ||= t; h[:k] << '!'",
  "multi_write" => "a, b = t; a << '!'",
  "pattern_capture" => "case t; in String => a; a << '!'; end" }.each do |name, body|
  cases[name] = [body, "", "abc!"]
end

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

def add_io_certificate_cases(certificate_cases)
  # IO's literal block is synchronous, but its separator lives across block
  # invocations. Frozen separators can use the retaining-box contract; a
  # mutable separator or an overridden/escaping call cannot borrow it.
  io_setup = <<~RUBY
    require "tmpdir"
    path = File.join(Dir.tmpdir, "spinel_forward_io_#{'#{Process.pid}'}")
    File.write(path, "one|two|tail")
  RUBY
  %w[each_line each].each do |method|
    certificate_cases["io_#{method}_frozen_rest"] = [io_setup + <<~RUBY, "[\"one\", \"two\", \"tail\"]\n[\"on\", \"e\", \"tw\", \"o\", \"ta\", \"il\"]\nzero limit\n", true]
      def read_lines(path, *args, **keywords)
        out = []
        File.open(path) do |file|
          file.#{method}(*args, **keywords) { |line| out << line }
        end
        out
      end
      p read_lines(path, "|", chomp: true)
      p read_lines(path, "|", 2, chomp: true)
      begin
        read_lines(path, "|", 0)
      rescue ArgumentError
        puts "zero limit"
      end
      File.delete(path)
    RUBY
  end
  certificate_cases["io_mutable_separator_callback"] = [io_setup + <<~RUBY, "\"|!!\"\n\"|!!\"\n", :native_or_refusal]
    def read_lines(path, *args)
      File.open(path) do |file|
        file.each_line(*args) { |line| args[0] << "!" }
      end
      nil
    end
    separator = +"|"
    other = separator
    read_lines(path, separator)
    p separator, other
    File.delete(path)
  RUBY
  certificate_cases["io_blockless_rest"] = [io_setup + <<~RUBY, "\"|!\"\n", false]
    def enumerate(file, *args)
      file.each_line(*args)
      nil
    end
    separator = +"|"
    File.open(path) { |file| enumerate(file, separator) }
    separator << "!"
    p separator
    File.delete(path)
  RUBY
  certificate_cases["io_escaping_nested_block"] = [io_setup + <<~RUBY, "\"|!\"\n", false]
    def read_lines(path, *args)
      File.open(path) do |file|
        file.each_line(*args) { |line| $reader = -> { args[0] } }
      end
      nil
    end
    separator = +"|"
    read_lines(path, separator)
    $reader.call << "!"
    p separator
    File.delete(path)
  RUBY
  %w[IO File].product(%w[each_line each]).each do |owner, method|
    certificate_cases["io_override_#{owner}_#{method}"] = [io_setup + <<~RUBY, "\"|!\"\n", false]
      class #{owner}
        def #{method}(*args)
          $saved = args[0]
          yield "line"
          self
        end
      end
      def read_lines(path, *args)
        File.open(path) { |file| file.#{method}(*args) { |line| nil } }
        nil
      end
      separator = +"|"
      read_lines(path, separator)
      $saved << "!"
      p separator
      File.delete(path)
    RUBY
  end
  # A break value, unlike an ordinary each_line block result, is the call's
  # result. Both a nested loop break and a non-tail File.open break must keep
  # the old capture proof until break-value flow is modeled independently.
  breaks = { "each_line" => "file.each_line { break value }", "open" => "break value; nil" }
  %w[any? all? none? one?].each do |method|
    breaks["predicate_#{method}"] = "[0].#{method} { break value; true }"
  end
  breaks.each do |name, body|
    entry = name.start_with?("predicate_") ? "relay" : "leak"
    certificate_cases["io_break_value_#{name}"] = [io_setup + <<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
      def leak(path, value)
        File.open(path) { |file| #{body} }
      end
      def relay(path, value) = leak(path, value)
      #{entry}(path, 1)
      text = +"ice"
      other = text
      #{entry}(path, text) << "!"
      p text, other
      File.delete(path)
    RUBY
  end
  certificate_cases["io_block_optional_default"] = [io_setup + <<~RUBY, "\"|!\"\n", :native_or_refusal]
    def read_lines(path, *args)
      File.open(path) do |file, copy = args[0]|
        file.each_line(*args) { |line| $saved = copy }
      end
      nil
    end
    separator = +"|"
    read_lines(path, separator)
    $saved << "!"
    p separator
    File.delete(path)
  RUBY
  certificate_cases["io_frozen_block_optional_default"] = [io_setup + <<~RUBY, "\"|\"\ntrue\n", :native_or_refusal]
    def read_lines(path, *args)
      File.open(path) do |file, copy = args[0]|
        file.each_line(*args) { |line| $saved = copy }
      end
      nil
    end
    read_lines(path, "|")
    p $saved, $saved.frozen?
    File.delete(path)
  RUBY
  path = File.expand_path("../test/io_each_line_splat_args.rb", __dir__)
  certificate_cases["original_io_each_line_splat_args"] = [File.read(path), File.binread(path + ".expected"), true]
  certificate_cases
end

def add_boxed_hash_certificate_cases(certificate_cases)
  # A boxed-value Hash preserves an unnarrowed incoming box. Its keys and
  # typed/narrowed values have different copy contracts; check metadata as
  # well as bytes so a copied frozen String cannot masquerade as preservation.
  { "string" => ['{"seed" => 0}', '"entry"'],
    "symbol" => ['{seed: 0}', ':entry'],
    "mixed" => ['{0 => 0}', ':entry'] }.each do |kind, (initial, key)|
    %w[aset store].each do |operation|
      write = operation == "aset" ? "@values[#{key}] = value" : "@values.store(#{key}, value)"
      holder = <<~RUBY
        class Holder
          def initialize = @values = #{initial}
          def put(value); #{write}; nil; end
          def get = @values[#{key}]
        end
        holder = Holder.new
        holder.put(1)
      RUBY
      certificate_cases["boxed_hash_#{kind}_#{operation}_frozen"] = [holder + <<~RUBY, "true\ntrue\nfrozen\n\"ice\"\n", true]
        source = "ice"
        holder.put(source)
        result = holder.get
        p result.equal?(source), result.frozen?
        begin
          result << "!"
        rescue FrozenError
          puts "frozen"
        end
        p source
      RUBY
      returned = holder.sub("#{write}; nil; end", "#{write}; end")
      certificate_cases["boxed_hash_#{kind}_#{operation}_returned"] = [returned + <<~RUBY, "true\ntrue\ntrue\nfrozen\n\"ice\"\n", true]
        source = "ice"
        result = holder.put(source)
        p result.equal?(source), result.equal?(holder.get), result.frozen?
        begin
          result << "!"
        rescue FrozenError
          puts "frozen"
        end
        p source
      RUBY
      certificate_cases["boxed_hash_#{kind}_#{operation}_mutable"] = [holder + <<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
        source = +"ice"
        holder.put(source)
        holder.get << "!"
        p source, holder.get
      RUBY
      # Occurrence-narrowing to String reboxes bytes even in a boxed Hash.
      narrowed = holder.sub(write, "if value.is_a?(String); #{write}; end")
      certificate_cases["boxed_hash_#{kind}_#{operation}_narrowed"] = [narrowed + <<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
        source = +"ice"
        holder.put(source)
        holder.get << "!"
        p source, holder.get
      RUBY
    end
  end
  %w[[]= store].each do |method|
    certificate_cases["boxed_hash_override_#{method}"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", false]
      $held = nil
      class Hash
        def #{method}(key, value); $held = value; value; end
      end
      class Holder
        def initialize = @values = {seed: 0}
        def put(value); @values.#{method}(:entry, value); nil; end
      end
      holder = Holder.new
      holder.put(1)
      source = +"ice"
      holder.put(source)
      $held << "!"
      p source, $held
    RUBY
    # A frozen actual would discharge BOX retention. The override's visible
    # side effect must not be hidden by store-to-[]= lowering or that discharge.
    certificate_cases["boxed_hash_frozen_override_#{method}"] = [<<~RUBY, "true\ntrue\n\"ice\"\n", :native_or_refusal]
      $held = nil
      class Hash
        def #{method}(key, value); $held = value; value; end
      end
      class Holder
        def initialize = @values = {seed: 0}
        def put(value); @values.#{method}(:entry, value); nil; end
      end
      holder = Holder.new
      holder.put(1)
      source = "ice"
      holder.put(source)
      p $held.equal?(source), $held.frozen?, source
    RUBY
  end
  path = File.expand_path("../test/boxed_param_value_into_typed_hash_slot.rb", __dir__)
  certificate_cases["original_boxed_param_value_into_typed_hash_slot"] = [File.read(path), File.binread(path + ".expected"), true]
  certificate_cases
end

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
    unless status.exitstatus == 1 && (out + err).match?(escape_path) &&
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
    unless status.exitstatus == 1 && (out + err).match?(escape_path) &&
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

  # Matz's three alias shapes at depth 0, 3 and 7. Seed with both nil and
  # Integer: the former can infer nullable String rather than POLY. Every
  # accepted program must preserve BOTH caller aliases; only the explicit
  # identity refusal (and no C) is an alternative. dup is always native.
  shapes = {
    "local_alias" => ["u = t; u << '!'", "abc!"],
    "indirect_alias" => ["u = t; v = u; v.replace('Z')", "Z"],
    "ternary_alias" => ["c = ARGV.empty?; (c ? t : t) << '!'", "abc!"],
    "dup" => ["copy = t.dup << '!'; p copy", "abc"]
  }
  shapes.each do |name, (body, want)|
    [0, 3, 7].each do |depth|
      %w[nil 1].each do |seed|
        label = "#{name}_depth#{depth}_seed#{seed}"
        source = File.join(dir, "#{label}.rb")
        cfile = File.join(dir, "#{label}.c")
        executable = File.join(dir, label)
        methods = (0...depth).map { |i| "def hop#{i}(t) = #{i + 1 == depth ? 'terminal' : "hop#{i + 1}"}(t)" }
        entry = depth.zero? ? "terminal" : "hop0"
        File.write(source, <<~RUBY)
          def terminal(t)
            return nil unless t.is_a?(String)
            #{body}
            nil
          end
          #{methods.join("\n")}
          #{entry}(#{seed})
          text = +'abc'
          other = text
          #{entry}(text)
          p text, other
        RUBY
        expected = (name == "dup" ? "\"abc!\"\n" : "") + "#{want.inspect}\n" * 2
        out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
        unless status.success? && out == expected
          failures << "#{label}: invalid CRuby reduction: #{out.inspect} #{err}"
          next
        end
        out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
        if status.exitstatus == 1 && name != "dup" &&
           (out + err).match?(escape_path) &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
          refusals += 1
          next
        end
        unless status.success? && File.exist?(cfile)
          failures << "#{label}: neither correct native path nor identity refusal: #{out}#{err}"
          next
        end
        out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
        if status.success?
          out, err, status = Open3.capture3(timeout, "30", executable)
          if status.success? && out == expected
            native_passes += 1
            next
          end
        end
        failures << "#{label}: native alias/copy result differs: #{out.inspect} #{err}"
      end
    end
  end

  # Shared += needs a rebinding proof, not an in-place append. These isolated
  # shapes test the actual statement arm, without an earlier Array refusal.
  { "plain" => ["'!'", "oldx!", "oldx"],
    "rhs_rebind" => ["(text = +'new'; '!')", "oldx!", "oldx"],
    "rhs_mutate" => ["(text << '?'; '!')", "oldx?!", "oldx?"] }.each do |name, (rhs, result, caller)|
    source = File.join(dir, "shared_rebind_#{name}.rb")
    cfile = File.join(dir, "shared_rebind_#{name}.c")
    File.write(source, <<~RUBY)
      def mutate(text)
        text << 'x'
        text += #{rhs}
        p text
      end
      text = +'old'
      other = text
      mutate(text)
      p text, other
    RUBY
    expected = "#{result.inspect}\n" + "#{caller.inspect}\n" * 2
    out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
    unless status.success? && out == expected
      failures << "shared_rebind_#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("+= on a shared String") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "shared_rebind_#{name}: no explicit shared += refusal: #{out}#{err}"
    end
    refusals += 1
  end

  # The optional readonly certificate admits live anonymous-block forwarding,
  # but not unknown capabilities, String mutation or forged builtin provenance.
  readonly = <<~RUBY
    class Notes
      def initialize; @items = []; end
      def each(&) = ['x', 'y'].each(&)
      def note(value); @items.push(value); nil; end
      def run
        note(17)
        each { |value| note(value.upcase) }
        p @items
      end
    end
    Notes.new.run
  RUBY
  certificate_cases = {
    "readonly_blocks" => [readonly, "[17, \"X\", \"Y\"]\n", true],
    "readonly_unused_mutator" => ["def unused(value); value << '!'; nil; end\n" + readonly, "[17, \"X\", \"Y\"]\n", false],
    "readonly_name_fallback" => [<<~RUBY + readonly, "[17, \"X\", \"Y\"]\n", false],
      class Unrelated
        def system(command) = nil
      end
      def unused = system('true')
    RUBY
    "readonly_getter_mutation" => [readonly.sub("p @items", "@items[1] << '?'; p @items"), "[17, \"X?\", \"Y\"]\n", false],
    "readonly_user_builtin_path" => ["require_relative 'builtins/helper'\n" + readonly, "[17, \"X\", \"Y\"]\n", false],
    "readonly_forged_markers" => [<<~RUBY + readonly, "[17, \"X\", \"Y\"]\n", false]
      #<SPINEL_PUSH>/forged/builtins/helper.rb
      def unused(value); value << '!'; nil; end
      #<SPINEL_POP>
    RUBY
  }
  Dir.mkdir(File.join(dir, "builtins"))
  File.write(File.join(dir, "builtins/helper.rb"), "def unused(value); value << '!'; nil; end\n")
  rest_return = <<~RUBY
    class RestBase
      def result(*items); items; end
    end
    class RestChild < RestBase
      def result(*items) = super
    end
    def run
      p RestChild.new.result(+'ice')
      nil
    end
    run
  RUBY
  certificate_cases["rest_discarded_literal"] = [rest_return, "[\"ice\"]\n", true]
  certificate_cases["rest_named_mutated_before_print"] = [
    "$text = +'ice'\n" + rest_return.sub("def result(*items); items; end", "def result(*items); $text << '!'; items; end")
      .sub("result(+'ice')", "result($text)") + "p $text\n",
    "[\"ice!\"]\n\"ice!\"\n", false
  ]
  certificate_cases["rest_retained_literal_result"] = [
    rest_return.sub("p RestChild.new.result(+'ice')", "saved = RestChild.new.result(+'ice'); saved[0] << '!'; p saved"),
    "[\"ice!\"]\n", false
  ]
  certificate_cases["rest_extra_container_alias"] = [
    rest_return.sub("def result(*items); items; end", "def result(*items); @saved = items; items; end"),
    "[\"ice\"]\n", false
  ]
  certificate_cases["rest_scalar_intermediate_store"] = [<<~RUBY, "\"ice\"\n", false]
    class Base
      def result(value) = value
    end
    class Middle < Base
      def result(value); @left = [value]; @right = [value]; super; end
    end
    class Child < Middle
      def result(*items) = super
    end
    def run
      p Child.new.result(+'ice')
      nil
    end
    run
  RUBY
  certificate_cases["rest_different_returned_formal"] = [<<~RUBY, "\"snow\"\n", false]
    class Base
      def result(a, b); @left = [a]; @right = [a]; b; end
    end
    class Child < Base
      def result(*items) = super
    end
    def run
      p Child.new.result(+'ice', +'snow')
      nil
    end
    run
  RUBY
  certificate_cases["rest_matching_returned_formal"] = [<<~RUBY, "\"ice\"\n", true]
    class Base
      def result(a, b) = b
    end
    class Child < Base
      def result(*items) = super
    end
    def run
      p Child.new.result(1, +'ice')
      nil
    end
    run
  RUBY
  certificate_cases["rest_post_and_unused_multiple_print"] = [<<~RUBY, "\"ice\"\n:post\n\"snow\"\n", true]
    class Base
      def result(a, *items, last) = last
    end
    class Child < Base
      def result(a, *items) = super
    end
    def run
      p Child.new.result(1, +'ice'), Child.new.result(1, +'unused', :post), Child.new.result(1, +'snow')
      nil
    end
    run
  RUBY
  certificate_cases["rest_reassigned_layout"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", false]
    class Base
      def result(a, b, *items); $saved = b; b; end
    end
    class Middle < Base
      def result(a, *items, last); items = []; super; end
    end
    class Child < Middle
      def result(a, *items) = super
    end
    def run
      p Child.new.result(1, :padding, +'ice'), ($saved << '!')
      nil
    end
    run
  RUBY
  certificate_cases["rest_terminal_default_escape"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", false]
    class Base
      def result(a, value, spare = ($saved = value)) = value
    end
    class Child < Base
      def result(a, *items) = super
    end
    def run
      p Child.new.result(1, +'ice'), ($saved << '!')
      nil
    end
    run
  RUBY
  # Only an audited preserving store permits literal provenance to bypass
  # reflection. A standalone POLY return keeps that contract; narrowing,
  # conversion and an unresolved rest destination do not.
  { "preserving_store" => ["def accept(value); store(value); nil; end", "nil", true],
    "boxed_return" => ["def accept(value) = store(value)", "value", true],
    "explicit_boxed_return" => ["def accept(value) = store(value)", "return value", true],
    "narrowed_return" => ["def accept(value); store(value); nil; end", "return value if value.is_a?(String); value", false],
    "boxed_to_s_result" => ["def accept(value); store(value); nil; end", "!ARGV.empty? ? value : value.to_s", false],
    "unresolved_rest" => ["def accept(*items); method(:store).call(*items); nil; end", "nil", false] }.each do |name, (accept, result, native)|
    certificate_cases["literal_#{name}_with_reflection"] = [<<~RUBY, "true\ntrue\ntrue\ntrue\nfrozen\n", native]
      class Keep
        def initialize = @items = []
        #{accept}
        def store(value); @items.push(value); @items.push(value); #{result}; end
        def at(index) = @items[index]
      end
      def unrelated = method(:p)
      k = Keep.new
      k.accept(1)
      2.times { k.accept("ice") }
      p k.at(2).equal?(k.at(3)), k.at(2).equal?(k.at(4))
      p k.at(2).frozen?, k.at(3).frozen?
      begin
        k.at(2) << "!"
      rescue FrozenError
        puts "frozen"
      end
    RUBY
  end
  # Returned values must themselves keep identity, not only the stored
  # aliases above. Ensure's deferred return and result temp are distinct ABIs.
  { "implicit" => "value", "explicit" => "return value",
    "explicit_ensure" => "begin; return value; ensure; puts :ensured; end",
    "implicit_ensure" => "begin; value; ensure; puts :ensured; end" }.each do |name, body|
    prefix = name.end_with?("ensure") ? "ensured\nensured\nensured\n" : ""
    certificate_cases["standalone_boxed_return_#{name}"] = [<<~RUBY, prefix + "true\ntrue\ntrue\nfrozen\n", true]
      class Echo
        def tail(value); #{body}; end
        def hop1(value) = tail(value)
        def hop2(value) = hop1(value)
        def hop3(value) = hop2(value)
        def unrelated = method(:tail)
      end
      e = Echo.new
      e.hop3(1)
      values = []
      2.times { values.push(e.hop3("ice")) }
      p values[0].equal?(values[1])
      p values[0].frozen?, values[1].frozen?
      begin
        values[0] << "!"
      rescue FrozenError
        puts "frozen"
      end
    RUBY
  end
  certificate_cases["discarded_poly_ivar_store"] = [<<~RUBY, "true\ntrue\ntrue\nfrozen\n", true]
    class Keep
      attr_reader :held
      def initialize = @held = nil
      def store(value); @held = value; nil; end
      def hop1(value) = store(value)
      def hop2(value) = hop1(value)
      def hop3(value) = hop2(value)
      def unrelated = method(:store)
    end
    k = Keep.new
    k.hop3(1)
    values = []
    2.times { k.hop3("ice"); values.push(k.held) }
    p values[0].equal?(values[1])
    p values[0].frozen?, values[1].frozen?
    begin
      values[0] << "!"
    rescue FrozenError
      puts "frozen"
    end
  RUBY
  certificate_cases["yielding_return_is_not_standalone"] = [<<~RUBY, "true\ntrue\n", false]
    def tail(value); yield; value; end
    def relay(value) = tail(value) { nil }
    relay(1)
    value = relay("ice")
    p value.frozen?, value == "ice"
  RUBY
  # A constructor discards initialize's value, but a normal yielding call can
  # splice it into a result slot. Treating both as discarded lost the aliases.
  certificate_cases["yielding_initializer_result"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n\"ice!\"\n", false]
    class Example
      def initialize(value = nil)
        yield if block_given?
        value
      end
      public :initialize
    end
    def relay(value)
      e = Example.new
      e.initialize(value) { nil }
    end
    relay(1)
    s = +"ice"
    other = s
    result = relay(s)
    result << "!"
    p s, other, result
  RUBY
  # No named + target is not a builtin-only proof: String#coerce can publish
  # the operand without defining an operator, then later observe its mutation.
  certificate_cases["arithmetic_coercion_publishes_operand"] = [<<~RUBY, "\"ab\"\n3\n0\n\"y!\"\n", false]
    class String
      def coerce(other)
        $saved = self
        [0, 0]
      end
    end
    def add(a, b) = a + b
    def relay(a, b) = add(a, b)
    p relay("a", "b")
    p relay(1, 2)
    s = +"y"
    p relay(1, s)
    s << "!"
    p $saved
  RUBY
  # The send's method-name operand must not shift the splat's destination.
  # A real prefix and keyword make the correct layout asymmetric.
  %w[send __send__ public_send].each do |dispatch|
    certificate_cases["dynamic_readonly_#{dispatch}"] = [<<~RUBY, "\"17:ice:z\"\n\"17:fire:y\"\n", true]
      class Sink
        def render(prefix, value, tag: :z) = "#{'#{prefix}:#{value}:#{tag}'}"
      end
      class Relay
        def initialize = @sink = Sink.new
        def go(name, *args, **kwargs) = @sink.#{dispatch}(name, 17, *args, **kwargs)
      end
      r = Relay.new
      p r.go(:render, "ice")
      p r.go(:render, "fire", tag: :y)
    RUBY
  end
  certificate_cases["dynamic_readonly_inherited_alias"] = [<<~RUBY, "\"17:ice\"\n", true]
    class Parent
      def render(prefix, value) = "#{'#{prefix}:#{value}'}"
    end
    class Sink < Parent
      alias deliver render
    end
    class Relay
      def initialize = @sink = Sink.new
      def go(name, *args) = @sink.send(name, 17, *args)
    end
    p Relay.new.go(:deliver, "ice")
  RUBY
  certificate_cases["forward_default_retention"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", false]
    class Reader
      def entry(value); leaf(value); nil; end
      def leaf(value, saved = ($held = value)); nil; end
    end
    r = Reader.new
    r.entry(0)
    source = +"ice"
    r.entry(source)
    source << "!"
    p source, $held
  RUBY
  certificate_cases["inline_class_method_namesake"] = [<<~RUBY, "3\nice!\n", true]
    def stash(value); $held = value; nil; end
    def deliver(value); stash(value); nil; end
    deliver(1)
    deliver(false)
    class Reader
      def self.deliver(value); puts value.length; yield; nil; end
      def self.run(value); deliver(value) {}; nil; end
    end
    source = +"ice"
    Reader.run(source)
    source << "!"
    puts source
  RUBY
  certificate_cases["inline_class_method_retention"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", false]
    class Reader
      def self.stash(value); $held = value; nil; end
      def self.deliver(value); stash(value); yield; nil; end
    end
    Reader.deliver(1) {}
    source = +"ice"
    Reader.deliver(source) {}
    source << "!"
    p source, $held
  RUBY
  certificate_cases["dynamic_prefixed_append"] = [<<~RUBY, "frozen\n", false]
    class Sink
      def leaf(prefix, value, unused = nil); value << "!"; nil; end
    end
    class Relay
      def initialize = @sink = Sink.new
      def go(name, *args) = @sink.send(name, 17, *args)
    end
    begin
      Relay.new.go(:leaf, "ice")
    rescue FrozenError
      puts "frozen"
    end
  RUBY
  certificate_cases["dynamic_mixed_arms"] = [<<~RUBY, "\"17:ice\"\nfrozen\n", false]
    class Sink
      def render(prefix, value) = "#{'#{prefix}:#{value}'}"
      def append(prefix, value); value << "!"; nil; end
    end
    class Relay
      def initialize = @sink = Sink.new
      def go(name, *args) = @sink.send(name, 17, *args)
    end
    r = Relay.new
    p r.go(:render, "ice")
    begin
      r.go(:append, "fire")
    rescue FrozenError
      puts "frozen"
    end
  RUBY
  # Resolving an arm is not permission to copy a retained/appended String.
  # Each reduction runs alone so an unsafe arm cannot hide behind another.
  {
    "literal_append" => ["def leaf(value); value << '!'; nil; end", <<~RUBY, "frozen\n"],
      begin
        r.go(:leaf, "ice")
      rescue FrozenError
        puts "frozen"
      end
    RUBY
    "mutable_append" => ["def leaf(value); value << '!'; nil; end", <<~RUBY, "\"ice!\"\n\"ice!\"\n"],
      source = +"ice"
      other = source
      r.go(:leaf, source)
      p source, other
    RUBY
    "stored_alias" => ["attr_reader :held; def leaf(value); @held = value; nil; end", <<~RUBY, "\"ice!\"\n\"ice!\"\n"],
      r.go(:leaf, 1)
      source = +"ice"
      r.go(:leaf, source)
      r.sink.held << "!"
      p source, r.sink.held
    RUBY
    "returned_alias" => ["def leaf(value) = value", <<~RUBY, "\"ice!\"\n\"ice!\"\n"],
      r.go(:leaf, 1)
      source = +"ice"
      result = r.go(:leaf, source)
      result << "!"
      p source, result
    RUBY
    "yielded_alias" => ["def leaf(value); yield value; nil; end", <<~RUBY, "frozen\n"],
      r.go(:leaf, 1) { |value| nil }
      begin
        r.go(:leaf, "ice") { |value| value << "!" }
      rescue FrozenError
        puts "frozen"
      end
    RUBY
    "post_rest" => ["def leaf(*unused, value); value << '!'; nil; end", <<~RUBY, "frozen\n"]
      begin
        r.go(:leaf, "ice")
      rescue FrozenError
        puts "frozen"
      end
    RUBY
  }.each do |name, (definition, calls, expected)|
    certificate_cases["dynamic_#{name}"] = [<<~RUBY, expected, false]
      class Sink
        #{definition}
      end
      class Relay
        attr_reader :sink
        def initialize = @sink = Sink.new
        def go(name, *args, &block) = @sink.send(name, *args, &block)
      end
      r = Relay.new
      #{calls}
    RUBY
  end
  certificate_cases["dynamic_capped_names"] = [<<~RUBY, "\"ice\"\n", false]
    class Sink
      def leaf(value) = "#{'#{value}'}"
    end
    class Relay
      def initialize = @sink = Sink.new
      def go(name, *args) = @sink.send(name, *args)
    end
    [#{270.times.map { |i| "\"unused_#{i}\"" }.join(', ')}]
    p Relay.new.go(:leaf, "ice")
  RUBY
  certificate_cases["dynamic_unresolved_builtin"] = [<<~RUBY, "frozen\n", false]
    class Relay
      def initialize = @receiver = "ice"
      def go(name, *args) = @receiver.send(name, *args)
    end
    begin
      Relay.new.go(:concat, "!")
    rescue FrozenError
      puts "frozen"
    end
  RUBY
  # The readonly certificate must not mistake a Hash's container-valued slot
  # for scalar-only storage. Mutate through the returned alias, not the input:
  # the direct-root retention exemption printed three "ice"s instead.
  certificate_cases["hash_op_write_nested_alias"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n\"ice!\"\n", false]
    def stash(value)
      h = { k: [] }
      h[:k] += [value]
      h
    end
    stash(1)
    s = +"ice"
    other = s
    h = stash(s)
    h[:k][0] << "!"
    p s, other, h[:k][0]
  RUBY
  { "plain" => ["{}", "h[:k] = [value]"],
    "store" => ["{}", "h.store(:k, [value])"],
    "or" => ["{}", "h[:k] ||= [value]"],
    "and" => ["{ k: [] }", "h[:k] &&= [value]"],
    "literal" => ["{ k: [value] }", "nil"] }.each do |name, (initial, write)|
    certificate_cases["hash_#{name}_nested_alias"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n\"ice!\"\n", false]
      def stash(value)
        h = #{initial}
        #{write}
        h
      end
      stash(1)
      s = +"ice"
      other = s
      h = stash(s)
      h[:k][0] << "!"
      p s, other, h[:k][0]
    RUBY
  end
  # Hash duplicates mutable String keys. That is correct Ruby semantics, not
  # copied-value retention: the stored-value rule must not reject a key read.
  { "literal" => "h = { key => [] }", "plain" => "h = {}; h[key] = []",
    "or" => "h = {}; h[key] ||= []" }.each do |name, body|
    certificate_cases["hash_mutable_key_#{name}"] = [<<~RUBY, "\"ice!\"\n\"ice\"\n", true]
      def stash(key)
        #{body}
        h
      end
      stash(1)
      key = +"ice"
      h = stash(key)
      key << "!"
      p key, h.keys[0]
    RUBY
  end
  # An Integer operand does not make String#<< readonly. Mixed Hash values
  # must invalidate the non-String fact even when the container arm is safe.
  certificate_cases["hash_boxed_integer_append"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
    def stash(value)
      h = { k: value }
      h[:k] << 33
      nil
    end
    stash([])
    s = +"ice"
    other = s
    stash(s)
    p s, other
  RUBY
  certificate_cases["indirect_builtin_method_mutation"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
    def relay(value)
      [value.method(:<<)][0].call("!")
      nil
    end
    def entry(value) = relay(value)
    entry([])
    s = +"ice"
    other = s
    entry(s)
    p s, other
  RUBY
  certificate_cases["mutating_optional_default"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
    def leaf(value, ignored = (value.is_a?(String) ? value << "!" : nil)) = nil
    def relay(value); leaf(value); nil; end
    relay(1)
    s = +"ice"
    other = s
    relay(s)
    p s, other
  RUBY
  certificate_cases["mutating_optional_keyword_default"] = [<<~RUBY, "\"ice!\"\n\"ice!\"\n", :native_or_refusal]
    def leaf(value, ignored: (value.is_a?(String) ? value << "!" : nil)) = nil
    def relay(value); leaf(value); nil; end
    relay(1)
    s = +"ice"
    other = s
    relay(s)
    p s, other
  RUBY
  # Owner metadata, not just class-block syntax, must exclude builtin reopenings.
  certificate_cases["builtin_singleton_owner"] = [<<~RUBY, "\"ice\"\n", :native_or_refusal]
    def String.harmless = nil
    def leaf(value) = [value]
    def relay(value) = leaf(value)
    relay(1)
    p relay("ice")[0]
  RUBY
  # Keep the original compatibility fixtures and their independently derived
  # expected outputs unchanged. Each runs alone so another program's mutation
  # or hidden protocol cannot disguise a certificate/dispatch regression.
  %w[method_capture_untyped_param_poly method_splat_float_arg
     poly_user_two_arg_aref hash_index_or_write_key_variant poly_dispatch_kwsplat].each do |name|
    path = File.expand_path("../test/#{name}.rb", __dir__)
    certificate_cases["original_#{name}"] = [File.read(path), File.binread(path + ".expected"), true]
  end
  cells = File.read(File.expand_path("../benchmark/bm_poly_cells.rb", __dir__))
  cells_expected = "count: 5\nhello\n42\n[3 items]\nworld\n99\n"
  certificate_cases["readonly_field_loop"] = [cells, cells_expected, true]
  certificate_cases["readonly_field_loop_coercion"] = [
    "class Hidden; def coerce(value) = [value, 1]; end\n" + cells, cells_expected, false
  ]
  add_boxed_hash_certificate_cases(add_io_certificate_cases(certificate_cases)).each do |name, (input, expected, native)|
    source = File.join(dir, "#{name}.rb")
    cfile = File.join(dir, "#{name}.c")
    executable = File.join(dir, name)
    File.write(source, input)
    out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
    unless status.success? && out == expected
      failures << "#{name}: invalid CRuby certificate reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    # Exclusion from an optional certificate does not require refusal when
    # the existing handle path already preserves the CRuby result. Exercise
    # the native output rather than treating mere C generation as safety.
    if native == :native_or_refusal && status.exitstatus == 1 &&
       (out + err).match?(escape_path) && (out + err).include?("nothing written") && !File.exist?(cfile)
      refusals += 1
      next
    end
    if !native
      unless status.exitstatus == 1 && (out + err).match?(escape_path) &&
             (out + err).include?("nothing written") && !File.exist?(cfile)
        failures << "#{name}: unsafe certificate admitted: #{out}#{err}"
      end
      refusals += 1
      next
    end
    if status.success?
      out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
      out, err, status = Open3.capture3(timeout, "30", executable) if status.success?
    end
    unless status.success? && out == expected
      failures << "#{name}: required native readonly result differs: #{out.inspect} #{err}"
    end
    native_passes += 1
  end

  # Frozen provenance permits preserving a box, not copying/thawing a String
  # returned through aliases or narrowing. Check identity and frozen state.
  { "alias" => "u = value; u << '!' if change; u",
    "narrowed" => "return value if value.is_a?(String); value",
    "forwarded" => "other(value)" }.each do |name, body|
    source = File.join(dir, "frozen_return_#{name}.rb")
    cfile = File.join(dir, "frozen_return_#{name}.c")
    executable = File.join(dir, "frozen_return_#{name}")
    File.write(source, <<~RUBY)
      # frozen_string_literal: true
      def other(value) = value
      def round_trip(value, change)
        #{body}
      end
      p round_trip(nil, false)
      source = 'ice'
      result = round_trip(source, false)
      p result.frozen?, result.equal?(source)
    RUBY
    expected = "nil\ntrue\ntrue\n"
    out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
    unless status.success? && out == expected
      failures << "frozen_return_#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    if status.exitstatus == 1 && (out + err).match?(escape_path) &&
       (out + err).include?("nothing written") && !File.exist?(cfile)
      refusals += 1
      next
    end
    if status.success?
      out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
      out, err, status = Open3.capture3(timeout, "30", executable) if status.success?
    end
    unless status.success? && out == expected
      failures << "frozen_return_#{name}: frozen identity result differs: #{out.inspect} #{err}"
    end
    native_passes += 1
  end

  # A concrete String ABI is not a boxed escape. This previously correct
  # tool-shaped chain must consume bytes without activating the POLY policy.
  source = File.join(dir, "concrete_file_join.rb")
  executable = File.join(dir, "concrete_file_join")
  File.write(source, <<~RUBY)
    def leaf(dir, name) = File.join(dir, name)
    def entry(name) = leaf('base', name)
    p entry('ice')
    text = +'tool'
    other = text
    p entry(text), text, other
  RUBY
  expected = "\"base/ice\"\n\"base/tool\"\n\"tool\"\n\"tool\"\n"
  out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
  if !status.success? || out != expected
    failures << "concrete_file_join: invalid CRuby control: #{out.inspect} #{err}"
  else
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
    out, err, status = Open3.capture3(timeout, "30", executable) if status.success?
    unless status.success? && out == expected
      failures << "concrete_file_join: required native result differs: #{out.inspect} #{err}"
    end
    native_passes += 1
  end

  # Hidden singleton aliases invalidate a scalar-formal caller census too,
  # even when its explicit direct calls happen to pass only frozen values.
  [false, true].each do |aliased|
    name = "frozen_class_formal#{aliased ? '_alias' : ''}"
    source = File.join(dir, "#{name}.rb")
    cfile = File.join(dir, "#{name}.c")
    executable = File.join(dir, name)
    File.write(source, <<~RUBY)
      # frozen_string_literal: true
      class Collector
        def initialize = @items = []
        def store(value); @items.push(value); nil; end
        def at(index) = @items[index]
      end
      class Entry
        def self.take(key, value)
          $collector.store(value) if value.is_a?(String)
          nil
        end
        #{aliased ? 'class << self; alias []= take; end' : ''}
      end
      $collector = Collector.new
      $collector.store(17)
      Entry.take(0, 0)
      text = 'ice'
      Entry.take(0, text)
      p $collector.at(1).frozen?, $collector.at(1).equal?(text)
    RUBY
    expected = "true\ntrue\n"
    out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
    unless status.success? && out == expected
      failures << "#{name}: invalid CRuby reduction: #{out.inspect} #{err}"
      next
    end
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    if aliased
      unless status.exitstatus == 1 && (out + err).match?(escape_path) &&
             (out + err).include?("nothing written") && !File.exist?(cfile)
        failures << "#{name}: hidden entry inherited a frozen census: #{out}#{err}"
      end
      refusals += 1
      next
    end
    if status.success?
      out, err, status = Open3.capture3(timeout, "30", compiler, source, "-o", executable)
      out, err, status = Open3.capture3(timeout, "30", executable) if status.success?
    end
    unless status.success? && out == expected
      failures << "#{name}: required native frozen result differs: #{out.inspect} #{err}"
    end
    native_passes += 1
  end

  source = File.expand_path("../test/reject/string_poly_stored_rebind.rb", __dir__)
  expected = File.binread(source + ".expected")
  out, err, status = Open3.capture3(RbConfig.ruby, "--enable-frozen-string-literal", source)
  if !status.success? || out != expected
    failures << "stored_rebind: invalid CRuby reference: #{out.inspect} #{err}"
  else
    cfile = File.join(dir, "stored_rebind.c")
    out, err, status = Open3.capture3(timeout, "30", compiler, source, "-c", "-o", cfile)
    unless status.exitstatus == 1 && (out + err).include?("+= on a shared String") &&
           (out + err).include?("nothing written") && !File.exist?(cfile)
      failures << "stored_rebind: no shared += refusal: #{out}#{err}"
    end
    refusals += 1
  end
end
abort failures.join("\n") unless failures.empty?
puts "forward-escape-check: #{refusals} independent CRuby-validated refusals, #{native_passes} native readonly/identity controls pass"
