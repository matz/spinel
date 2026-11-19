# Spinel bundled `pp` -- Kernel#pp and #pretty_inspect, laid out by
# prettyprint (CRuby's own, unchanged) the way CRuby's pp lays them out: a
# value wider than the line breaks across lines, and an object's instance
# variables are listed sorted.
#
# CRuby's pp.rb picks each object's layout by reflection a compiled program
# does not have (Method#owner, UnboundMethod#bind_call, ObjectSpace). This one
# takes the same layouts by asking what the program can answer:
#
# * Array, Hash (Ruby 3.4's `key: value`), String (a multi-line one as
#   `"a\n" + "b"`), Range, Struct and Data are laid out as CRuby's are
# * an object whose class (or an ancestor below Object) defines `inspect`
#   prints that; any other object with instance variables is
#   `#<Class:0x... @a=..., @b=...>`, broken across lines when it is wide
# * a class's own `pretty_print(q)` is called, as CRuby calls it
# * a value seen again inside itself prints `[...]`, `{...}` or
#   `#<Class:0x... ...>`
#
# Left out: Set, ENV, File::Stat, MatchData and the AST node layouts (they
# print as `inspect` does); `pretty_print_cycle`,
# `pretty_print_instance_variables` and `pretty_print_inspect`;
# PP::PPMethods, PP::ObjectMixin, PP::SingleLine and PP.singleline_pp;
# PP.sharing_detection and Delegator unwrapping. The width is $COLUMNS or 80, less one, as CRuby's is
# when the output is not a terminal; a terminal's own width is not asked.
require "prettyprint"

class PP < PrettyPrint
  VERSION = "0.6.3"

  def PP.width_for(out)
    (ENV["COLUMNS"]&.to_i&.nonzero? || 80) - 1
  end

  # PP.new takes what PrettyPrint.new takes: the output, the width and the
  # newline, as CRuby's does (a library makes its printer with
  # `PP.new(out, width)`). PP.pp builds the layout in a String of its own,
  # read back through #output and then appended to `out`, whatever `out`
  # is.
  def initialize(output = String.new, maxwidth = 79, newline = "\n")
    super(output, maxwidth, newline)
  end

  def PP.pp(obj, out = $stdout, width = width_for(out))
    q = new(String.new, width)
    q.pp obj
    q.flush
    out << q.output << "\n"
  end

  # CRuby's guard_inspect_key sets up the table that finds a value inside
  # itself; the check here is PP's own (@pp_seen), so the guard runs the
  # block.
  def guard_inspect_key
    yield
  end

  def pp(obj)
    @pp_seen ||= []
    if @pp_seen.any? { |o| o.equal?(obj) }
      pp_cycle obj
      return
    end
    @pp_seen << obj
    begin
      group { pp_value obj }
    ensure
      @pp_seen.pop
    end
  end

  def pp_value(obj)
    case obj
    when Numeric, Symbol, true, false, nil, Module
      text obj.inspect
    when Array
      group(1, "[", "]") { seplist(obj) { |v| pp v } }
    when Hash
      pp_hash obj
    when String
      pp_string obj
    when Range
      pp_range obj
    when Struct
      pp_members "#<struct #{obj.class.name}", obj.members, obj.to_a
    when Data
      h = obj.to_h
      # no space after "data" for a class with no name, as CRuby's
      # Data#pretty_print; its Struct#pretty_print keeps the space
      name = obj.class.name
      pp_members(name ? "#<data #{name}" : "#<data", h.keys, h.values)
    else
      if obj.respond_to?(:pretty_print)
        obj.pretty_print(self)
      elsif pp_address(obj) && !obj.instance_variables.empty?
        pp_object obj
      else
        text obj.inspect
      end
    end
  end

  def pp_cycle(obj)
    case obj
    when Array then text(obj.empty? ? "[]" : "[...]")
    when Hash then text(obj.empty? ? "{}" : "{...}")
    when Struct then text "#<struct #{obj.class.name}:...>"
    else object_address_group(obj) { breakable; text "..." }
    end
  end

  # `#<Name:0x...` when obj's inspect is the default one, which starts
  # with it; nil when its class (or an ancestor) defines its own
  def pp_address(obj)
    name = obj.class.name
    return nil unless name
    ins = obj.inspect
    head = "#<#{name}:0x"
    return nil unless ins.start_with?(head)
    i = head.length
    i += 1 while i < ins.length && ins[i] != " " && ins[i] != ">"
    ins[0, i]
  end

  def object_group(obj, &block)
    group(1, "#<" + obj.class.name, ">", &block)
  end

  def object_address_group(obj, &block)
    head = pp_address(obj)
    head = obj.to_s.chomp(">") unless head
    group(1, head, ">", &block)
  end

  def comma_breakable
    text ","
    breakable
  end

  def seplist(list, sep = nil)
    first = true
    list.each do |v|
      if first
        first = false
      elsif sep
        sep.call
      else
        comma_breakable
      end
      yield v
    end
  end

  def pp_object(obj)
    object_address_group(obj) {
      names = obj.instance_variables.sort
      pp_named names.map(&:to_s), names.map { |v| obj.instance_variable_get(v) }
    }
  end

  # `name=value` pairs, a comma between them, each on its own line when
  # they do not fit (an object's ivars, a Struct's or a Data's members)
  def pp_named(names, values)
    names.each_with_index do |name, i|
      text "," if i > 0
      breakable
      text name
      text "="
      group(1) {
        breakable ""
        pp values[i]
      }
    end
  end

  def pp_hash(obj)
    group(1, "{", "}") {
      seplist(obj.to_a) { |kv|
        group { pp_hash_pair kv[0], kv[1] }
      }
    }
  end

  def pp_hash_pair(k, v)
    if k.is_a?(Symbol)
      ki = k.inspect
      if ki.match?(%r[\A:["$@!]|[%&*+\-\/<=>@\]^`|~]\z])
        text "#{k.to_s.inspect}:"
      else
        text "#{ki[1..]}:"
      end
    else
      pp k
      text " "
      text "=>"
    end
    group(1) {
      breakable
      pp v
    }
  end

  def pp_string(obj)
    lines = obj.lines
    if lines.size > 1
      group(0, "", "") do
        seplist(lines, lambda { text " +"; breakable }) { |v| pp v }
      end
    else
      text obj.inspect
    end
  end

  def pp_range(obj)
    begin_nil = obj.begin.nil?
    end_nil = obj.end.nil?
    pp obj.begin if !begin_nil || end_nil
    breakable ""
    text(obj.exclude_end? ? "..." : "..")
    breakable ""
    pp obj.end if !end_nil || begin_nil
  end

  def pp_members(head, names, values)
    group(1, head, ">") { pp_named names.map(&:to_s), values }
  end
end

module Kernel
  def pretty_inspect
    PP.pp(self, "".dup)
  end

  def pp(*objs)
    objs.each { |obj| PP.pp(obj) }
    objs.size <= 1 ? objs.first : objs
  end
end
