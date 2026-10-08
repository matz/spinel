# Spinel package: optparse
#
# A statically typable subset of CRuby's OptionParser:
#   - OptionParser.new(banner, width, indent) { |opts| ... }
#   - on / on_tail with any number of switch names ("-nNAME" declares -n with
#     a value), any number of description lines and an optional value type
#     (String, Array, Integer, Float, TrueClass or FalseClass), in any order
#   - separator, banner=, summary_width, summary_indent, to_s (help text)
#   - parse! with --long=VALUE, --long VALUE, -s VALUE, -sVALUE, clustered
#     short switches (-vq, -vuNAME) and "--"
#   - --[no-]name switches: --name passes true, --no-name passes false;
#     with a required value, only the positive form reads that value
#   - optional values: "--name[=VALUE]" and "-n[VALUE]" take only an attached
#     value; "--name [VALUE]" also takes the next word unless it looks like a
#     switch. Without a value the block gets nil
#   - abbreviated long switches: "--verb" is "--verbose", each word may be
#     shortened ("--d-r" is "--dry-run") and case is ignored; a name two
#     switches share raises AmbiguousOption
#   - Integer reads 12, -3, 0x1f, 0b11, 010 and 1_000, and Float reads 1.5,
#     -.5 and 1e3; any other word raises OptionParser::InvalidArgument
#   - String refuses an empty value with OptionParser::InvalidArgument, and
#     Array passes nil for an empty item ("a,,b"); without a type an empty
#     value passes as it is
#   - TrueClass and FalseClass read yes, true and + as true, and no, false,
#     - and nil as false, each word shortened as far as it stays clear ("y",
#     "n"). Without a value, TrueClass passes true and FalseClass false
#   - OptionParser::InvalidOption, OptionParser::AmbiguousOption,
#     OptionParser::MissingArgument, OptionParser::NeedlessArgument,
#     OptionParser::InvalidArgument and its subclass
#     OptionParser::AmbiguousArgument, all subclasses of
#     OptionParser::ParseError
#
# Not supported: other value types than String, Array, Integer, Float,
# TrueClass and FalseClass.

class OptionParser
  class ParseError < StandardError
  end

  class InvalidOption < ParseError
  end

  class AmbiguousOption < ParseError
  end

  class MissingArgument < ParseError
  end

  class NeedlessArgument < ParseError
  end

  class InvalidArgument < ParseError
  end

  class AmbiguousArgument < InvalidArgument
  end

  # The classes on takes as a switch's value type.
  VALUE_TYPES = [String, Array, Integer, Float, TrueClass, FalseClass]

  # The words a TrueClass or a FalseClass switch reads, and their values.
  BOOLEANS = { "+" => true, "-" => false, "yes" => true, "no" => false,
               "true" => true, "false" => false, "nil" => false }

  # The words an Integer or a Float switch accepts. radix is a leading 0
  # with an octal, binary (0b) or hexadecimal (0x) number.
  digits = '\d+(?:_\d+)*'
  radix = '0(?:[0-7]+(?:_[0-7]+)*|b[01]+(?:_[01]+)*|x[\da-f]+(?:_[\da-f]+)*)?'
  INTEGER_VALUE = /\A[-+]?(?:#{radix}|#{digits})\z/io
  FLOAT_VALUE = /\A[-+]?(?:#{digits}(?:\.(?:#{digits})?)?|\.#{digits})
                 (?:E[-+]?#{digits})?\z/iox

  # One entry of the help text: a switch, or a separator line (no names).
  class Switch
    attr_reader :shorts, :longs, :arg, :descriptions, :handler, :type

    def initialize(shorts, longs, arg, descriptions, handler, type)
      @shorts = shorts
      @longs = longs
      @arg = arg
      @descriptions = descriptions
      @handler = handler
      @type = type
    end

    def takes_value
      @arg != ""
    end

    # "[=VALUE]", "=[VALUE]" or "[VALUE]": only an attached value.
    def optional_value?
      @arg.start_with?("[") || @arg.start_with?("=[")
    end

    # " [VALUE]": an attached value, or else the next word.
    def placed_value?
      @arg.start_with?(" ") && @arg.lstrip.start_with?("[")
    end

    def separator?
      @shorts.empty? && @longs.empty?
    end

    def matches?(name)
      @shorts.include?(name) || @longs.any? { |long| accepts?(long, name) }
    end

    # A --[no-]name declaration accepts --name and --no-name; any other long
    # declaration accepts only itself.
    def accepts?(long, name)
      return long == name unless long.start_with?("--[no-]")

      base = long.delete_prefix("--[no-]")
      name == "--" + base || name == "--no-" + base
    end

    def negated?(name)
      @longs.any? do |long|
        long.start_with?("--[no-]") &&
          "--no-" + long.delete_prefix("--[no-]") == name
      end
    end
  end

  attr_accessor :banner, :summary_width, :summary_indent

  def initialize(banner = nil, width = 32, indent = "    ", &block)
    @banner = banner || "Usage: " + File.basename($0) + " [options]"
    @summary_width = width
    @summary_indent = indent
    @entries = []
    @tail = []
    block.call(self) if block
  end

  def separator(text)
    @entries.push(Switch.new([], [], "", [text], nil, Object))
  end

  def on(*args, &block)
    @entries.push(build_switch(args, block))
  end

  def on_tail(*args, &block)
    @tail.push(build_switch(args, block))
  end

  # When a switch raises an error, argv keeps only the words after the
  # switch that failed and the value it read, as in CRuby.
  def parse!(argv = ARGV)
    rest = []
    i = 0
    begin
      while i < argv.length
        @used = i
        arg = argv[i]
        if arg == "--"
          rest.concat(argv[(i + 1)..])
          break
        end
        if arg.length > 2 && arg[0] == "-" && arg[1] == "-"
          i = parse_long(argv, i)
        elsif arg.length > 1 && arg[0] == "-"
          i = parse_short(argv, i)
        else
          rest.push(arg)
        end
        i += 1
      end
    rescue ParseError
      rest = argv[(@used + 1)..]
      argv.clear
      argv.concat(rest)
      raise
    end
    argv.clear
    argv.concat(rest)
    argv
  end

  def parse(argv)
    parse!(argv.dup)
  end

  def to_s
    out = @banner + "\n"
    (@entries + @tail).each { |e| out += help_line(e) }
    out
  end

  alias help to_s

  private

  def build_switch(args, block)
    shorts = []
    longs = []
    arg_text = ""
    descriptions = []
    type = Object
    args.each do |a|
      if a.is_a?(String) && a.length > 1 && a[0] == "-"
        # a "[" opens an optional value ("--name[=VALUE]"), but the "[no-]" of
        # a negatable long switch is part of its name
        from = a.start_with?("--[no-]") ? 7 : 1
        cut = a.index(/[=\[ ]/, from) || (a[1] == "-" ? a.length : 2)
        text = a[cut..]
        arg_text = text unless text.empty?
        (a[1] == "-" ? longs : shorts).push(a[0, cut])
      elsif a.is_a?(String)
        descriptions.push(a)
      elsif VALUE_TYPES.include?(a)
        type = a
      end
    end
    Switch.new(shorts, longs, arg_text, descriptions, block, type)
  end

  def help_line(sw)
    return sw.descriptions[0] + "\n" if sw.separator?
    names = (sw.shorts + sw.longs).join(", ") + sw.arg
    names = "    " + names if sw.shorts.empty?
    descriptions = sw.descriptions
    return @summary_indent + names + "\n" if descriptions.empty?
    gap = @summary_indent + " " * (@summary_width + 1)
    out = @summary_indent + names.ljust(@summary_width) + " "
    out = @summary_indent + names + "\n" + gap if names.length > @summary_width
    out += descriptions[0] + "\n"
    descriptions[1..].each { |d| out += gap + d + "\n" }
    out
  end

  def find_switch(name)
    (@entries + @tail).find { |e| e.matches?(name) }
  end

  # Passes the value to the block in the switch's type. An empty String or
  # a word that is not an Integer, a Float or a boolean raises
  # InvalidArgument naming it as given (shown). Object is the type of a
  # switch declared without one. A boolean switch with "[=VALUE]" and no
  # value passes its own default.
  def invoke(sw, value, shown)
    handler = sw.handler
    type = sw.type
    boolean = type == TrueClass || type == FalseClass
    if value.nil? && boolean && sw.optional_value?
      handler.call(type == TrueClass) if handler
    elsif value.nil? || type == Object
      handler.call(value) if handler
    elsif boolean
      handler.call(complete_value(value, BOOLEANS, shown)) if handler
    elsif type == String
      raise invalid_argument(shown) if value.empty?
      handler.call(value) if handler
    elsif type == Array
      handler.call(value.split(",").map { |item| item.empty? ? nil : item }) if handler
    elsif type == Integer
      raise invalid_argument(shown) unless value.match?(INTEGER_VALUE)
      begin
        number = Integer(value)
      rescue ArgumentError
        raise invalid_argument(shown)
      end
      handler.call(number) if handler
    else
      raise invalid_argument(shown) unless value.match?(FLOAT_VALUE)
      handler.call(value.to_f) if handler
    end
  end

  def invalid_argument(shown)
    InvalidArgument.new("invalid argument: " + shown)
  end

  # A FalseClass flag passes false in both of its forms.
  def invoke_flag(sw, value)
    handler = sw.handler
    handler.call(value && sw.type != FalseClass) if handler
  end

  # Returns the value of the key of choices that word names, completed like
  # a long switch name ("y" is "yes") but with case counting. An exact key
  # wins. Keys with the same value never conflict, and the shortest key wins
  # if it starts all the others; otherwise raises AmbiguousArgument. Raises
  # InvalidArgument when no key matches.
  def complete_value(word, choices, shown)
    return choices[word] if choices.key?(word)
    pattern = Regexp.new("\\A" + Regexp.quote(word).gsub(/\w+\b/, "\\&\\w*"))
    found = choices.keys.select { |key| key.match?(pattern) }.sort_by(&:length)
    raise invalid_argument(shown) if found.empty?
    best = found[0]
    value = choices[best]
    found.each do |key|
      next if choices[key] == value || key.start_with?(best)
      raise AmbiguousArgument.new("ambiguous argument: " + shown)
    end
    value
  end

  # Returns the next word as the value of a switch with no attached value.
  # An optional value is nil instead: "[=VALUE]" never takes the next word,
  # and " [VALUE]" leaves it when it looks like a switch, as in CRuby.
  # Raises MissingArgument when a required value has no next word.
  def next_value(sw, argv, index, name)
    return nil if sw.optional_value?
    word = index + 1 < argv.length ? argv[index + 1] : nil
    if sw.placed_value?
      return nil if word.nil? || word.match?(/\A-./)
      return word
    end
    raise MissingArgument.new("missing argument: " + name) if word.nil?
    word
  end

  # Returns the full name of a long switch from a shortened one. Each word
  # may be cut ("--d-r" is "--dry-run") and case is ignored; an exact name
  # wins. When names of several switches match, the shortest wins if it
  # starts all the others ("--lis" is "--list" beside "--listen"), else
  # raises AmbiguousOption. Switches from on come before on_tail ones, and
  # a bare "--" matches nothing.
  def complete_long(name)
    return name if find_switch(name)
    return nil if name == "--"
    words = Regexp.quote(name[2..]).gsub(/\w+\b/, "\\&\\w*")
    pattern = Regexp.new("\\A" + words, Regexp::IGNORECASE)
    complete_in(@entries, name, pattern) || complete_in(@tail, name, pattern)
  end

  # complete_long within one list; nil when nothing matches.
  def complete_in(entries, name, pattern)
    found = []
    entries.each do |sw|
      sw.longs.each do |long|
        if long.start_with?("--[no-]")
          base = long.delete_prefix("--[no-]")
          found.push(["--" + base, sw]) if base.match?(pattern)
          found.push(["--no-" + base, sw]) if ("no-" + base).match?(pattern)
        elsif long[2..].match?(pattern)
          found.push([long, sw])
        end
      end
    end
    return nil if found.empty?
    found = found.sort_by { |pair| pair[0].length }
    best, best_sw = found[0]
    found.each do |full, sw|
      next if sw == best_sw || full.start_with?(best)
      raise AmbiguousOption.new("ambiguous option: " + name)
    end
    best
  end

  # Returns the index of the last word used, so parse! skips a value word.
  def parse_long(argv, index)
    arg = argv[index]
    eq = arg.index("=")
    name = eq ? arg[0, eq] : arg
    full = complete_long(name)
    raise InvalidOption.new("invalid option: " + name) if full.nil?
    sw = find_switch(full)
    is_enabled = !sw.negated?(full)
    if sw.takes_value && is_enabled
      attached = eq ? arg[(eq + 1)..] : nil
      value = attached || next_value(sw, argv, index, name)
      index += 1 if attached.nil? && value
      @used = index
      invoke(sw, value, attached ? arg : name + " " + value.to_s)
    else
      raise NeedlessArgument.new("needless argument: " + arg) if eq
      invoke_flag(sw, is_enabled)
    end
    index
  end

  # Reads each letter after the dash as one switch. A switch that takes a
  # value uses the rest of the word. Errors name the word from the failing
  # letter on, as in CRuby.
  # Returns the index of the last word used, so parse! skips a value word.
  def parse_short(argv, index)
    arg = argv[index]
    pos = 1
    while pos < arg.length
      name = "-" + arg[pos]
      from_here = "-" + arg[pos..]
      sw = find_switch(name)
      raise InvalidOption.new("invalid option: " + from_here) if sw.nil?
      if sw.takes_value
        attached = pos + 1 < arg.length ? arg[(pos + 1)..] : nil
        value = attached || next_value(sw, argv, index, name)
        index += 1 if attached.nil? && value
        @used = index
        invoke(sw, value, attached ? from_here : name + " " + value.to_s)
        break
      end
      raise NeedlessArgument.new("needless argument: " + from_here) if arg[pos + 1] == "="
      invoke_flag(sw, true)
      pos += 1
    end
    index
  end
end
