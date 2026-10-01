# Spinel package: optparse
#
# A statically typable subset of CRuby's OptionParser:
#   - OptionParser.new(banner, width, indent) { |opts| ... }
#   - on / on_tail with any number of switch names ("-nNAME" declares -n with
#     a value), any number of description lines and an optional Array type,
#     in any order
#   - separator, banner=, summary_width, summary_indent, to_s (help text)
#   - parse! with --long=VALUE, --long VALUE, -s VALUE, -sVALUE, clustered
#     short switches (-vq, -vuNAME) and "--"
#   - OptionParser::InvalidOption, OptionParser::MissingArgument and
#     OptionParser::NeedlessArgument, all subclasses of
#     OptionParser::ParseError
#
# Not supported: abbreviated long switches, optional values, --[no-] forms,
# other value types than String and Array.

class OptionParser
  class ParseError < StandardError
  end

  class InvalidOption < ParseError
  end

  class MissingArgument < ParseError
  end

  class NeedlessArgument < ParseError
  end

  # One entry of the help text: a switch, or a separator line (no names).
  class Switch
    attr_reader :shorts, :longs, :arg, :descriptions, :handler, :is_array

    def initialize(shorts, longs, arg, descriptions, handler, is_array)
      @shorts = shorts
      @longs = longs
      @arg = arg
      @descriptions = descriptions
      @handler = handler
      @is_array = is_array
    end

    def takes_value
      @arg != ""
    end

    def separator?
      @shorts.empty? && @longs.empty?
    end

    def matches?(name)
      @shorts.include?(name) || @longs.include?(name)
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
    @entries.push(Switch.new([], [], "", [text], nil, false))
  end

  def on(*args, &block)
    @entries.push(build_switch(args, block))
  end

  def on_tail(*args, &block)
    @tail.push(build_switch(args, block))
  end

  # When a switch raises an error, argv keeps only the words after the
  # switch that failed, as in CRuby.
  def parse!(argv = ARGV)
    rest = []
    i = 0
    begin
      while i < argv.length
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
      rest = argv[(i + 1)..]
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
    is_array = false
    args.each do |a|
      if a.is_a?(String) && a.length > 1 && a[0] == "-"
        cut = a.index(/[= ]/) || (a[1] == "-" ? a.length : 2)
        text = a[cut..]
        arg_text = text unless text.empty?
        (a[1] == "-" ? longs : shorts).push(a[0, cut])
      elsif a.is_a?(String)
        descriptions.push(a)
      elsif a == Array
        is_array = true
      end
    end
    Switch.new(shorts, longs, arg_text, descriptions, block, is_array)
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

  def invoke(sw, value)
    handler = sw.handler
    return if handler.nil?
    if sw.is_array
      handler.call(value.split(","))
    else
      handler.call(value)
    end
  end

  def invoke_flag(sw)
    handler = sw.handler
    handler.call(true) if handler
  end

  # Returns the attached value if there is one, otherwise the next word.
  # Raises MissingArgument when neither exists.
  def read_value(argv, index, attached, name)
    return attached if attached
    raise MissingArgument.new("missing argument: " + name) if index + 1 >= argv.length
    argv[index + 1]
  end

  # Returns the index of the last word used, so parse! skips a value word.
  def parse_long(argv, index)
    arg = argv[index]
    eq = arg.index("=")
    name = eq ? arg[0, eq] : arg
    sw = find_switch(name)
    raise InvalidOption.new("invalid option: " + name) if sw.nil?
    if sw.takes_value
      attached = eq ? arg[(eq + 1)..] : nil
      invoke(sw, read_value(argv, index, attached, name))
      index += 1 if attached.nil?
    else
      raise NeedlessArgument.new("needless argument: " + arg) if eq
      invoke_flag(sw)
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
        invoke(sw, read_value(argv, index, attached, name))
        index += 1 if attached.nil?
        break
      end
      raise NeedlessArgument.new("needless argument: " + from_here) if arg[pos + 1] == "="
      invoke_flag(sw)
      pos += 1
    end
    index
  end
end
