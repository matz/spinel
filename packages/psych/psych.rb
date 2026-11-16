# Spinel bundled `psych` -- YAML for a Spinel program, written in Ruby.
#
# CRuby's Psych is libyaml (C) under a Ruby layer that turns its events into
# objects. This package is both halves in Ruby: a parser that reads the YAML a
# program keeps its configuration in and answers what Psych.safe_load answers,
# and an emitter that writes what Psych.dump writes. The scalar rules are
# Psych's own (Psych::ScalarScanner, copied below), and the emitter makes
# libyaml's choices of style, quoting, folding and indentation, so a document
# reads back the same and a dump is byte for byte CRuby's.
#
# What it reads: block mappings and sequences (an indentless sequence under a
# key included), flow [a, b] and {a: 1}, plain, single- and double-quoted
# scalars (folding and escapes), literal and folded block scalars with their
# chomping and indentation indicators, comments, anchors and aliases, merge
# keys (<<), explicit `? key`, `---` / `...` and the first document of a
# stream. Psych::SyntaxError, DisallowedClass, AliasesNotEnabled and
# AnchorNotDefined are raised where Psych raises them.
#
# Tags read as Psych's ToRuby reads them. A tag naming a class (!ruby/object,
# !ruby/regexp, !!set, a timestamp...) asks the class loader first, so an
# unpermitted class raises DisallowedClass as in Psych. Not here, raised as
# NotImplementedError where a program reaches them: building such an object
# once the load permits its class (a Date or Time included) or under
# unsafe_load, an Encoding (!ruby/encoding), an ordered map (!!omap),
# `freeze: true`, dumping a value Psych would tag (a Range, a program's own
# object), and a collection as a mapping key in a dump. Psych.parse and
# the event and node APIs (Psych::Nodes, Psych::Handler) are not provided.

module Psych
  VERSION = "5.3.1"
  LIBYAML_VERSION = "0.2.5"

  class Exception < RuntimeError
  end

  class BadAlias < Exception
  end

  class AliasesNotEnabled < BadAlias
    def initialize
      super "Alias parsing was not enabled. To enable it, pass `aliases: true` to `Psych::load` or `Psych::safe_load`."
    end
  end

  class AnchorNotDefined < BadAlias
    def initialize(anchor_name)
      super "An alias referenced an unknown anchor: #{anchor_name}"
    end
  end

  class DisallowedClass < Exception
    def initialize(action, klass_name)
      super "Tried to #{action} unspecified class: #{klass_name}"
    end
  end

  class SyntaxError < Exception
    attr_reader :file, :line, :column, :offset, :problem, :context

    def initialize(file, line, col, offset, problem, context)
      err = context ? "#{problem} #{context}" : problem
      super("(#{file || '<unknown>'}): #{err} at line #{line} column #{col}")
      @file = file
      @line = line
      @column = col
      @offset = offset
      @problem = problem
      @context = context
    end
  end

  # Psych::ScalarScanner's patterns, verbatim but for the /x layout.
  TIME_RE = /^-?\d{4}-\d{1,2}-\d{1,2}(?:[Tt]|\s+)\d{1,2}:\d\d:\d\d(?:\.\d*)?(?:\s*(?:Z|[-+]\d{1,2}:?(?:\d\d)?))?$/
  FLOAT_RE = /^(?:[-+]?([0-9][0-9_,]*)?\.[0-9]*([eE][-+][0-9]+)?)$/
  INTEGER_STRICT_RE = /^(?:[-+]?0b[_]*[0-1][0-1_]*|[-+]?0[_]*[0-7][0-7_]*|[-+]?(0|[1-9][0-9_]*)|[-+]?0x[_]*[0-9a-fA-F][0-9a-fA-F_]*)$/
  INTEGER_LEGACY_RE = /^(?:[-+]?0b[_,]*[0-1][0-1_,]*|[-+]?0[_,]*[0-7][0-7_,]*|[-+]?(?:0|[1-9](?:[0-9]|,[0-9]|_[0-9])*)|[-+]?0x[_,]*[0-9a-fA-F][0-9a-fA-F_,]*)$/
  STRINGISH_RE = %r{^[^\d.:-]?[[:alpha:]_\s!@#$%\^&*(){}<>|/\\~;=]+}
  DATE_RE = /^\d{4}-(?:1[012]|0\d|\d)-(?:[12]\d|3[01]|0\d|\d)$/
  SEXAGESIMAL_INT_RE = /^[-+]?[0-9][0-9_]*(:[0-5]?[0-9]){1,2}$/
  SEXAGESIMAL_FLOAT_RE = /^[-+]?[0-9][0-9_]*(:[0-5]?[0-9]){1,2}\.[0-9_]*$/

  # Which classes and symbols a load may build (Psych::ClassLoader::Restricted)
  # and how its plain scalars resolve (Psych::ScalarScanner#tokenize).
  class SpResolver
    def initialize(permitted_classes, permitted_symbols, strict_integer, parse_symbols, for_dump = false, unsafe = false)
      @classes = permitted_classes.map { |c| c.to_s }
      # Psych.unsafe_load: no restriction (Psych::ClassLoader, not ::Restricted)
      @unsafe = unsafe
      @symbols = permitted_symbols.map { |s| s.to_s }
      @strict_integer = strict_integer
      @parse_symbols = parse_symbols
      # Psych's dump scans with an unrestricted class loader: a date or a
      # time is not a String there, whatever a load permits
      @for_dump = for_dump
    end

    def permits?(name)
      @unsafe || @classes.include?(name)
    end

    def symbolize(name)
      unless @unsafe
        raise DisallowedClass.new("load", "Symbol") unless @classes.include?("Symbol")
        raise DisallowedClass.new("load", "Symbol") unless @symbols.empty? || @symbols.include?(name)
      end
      name.to_sym
    end

    # A node that Psych builds as an object of the class `name` (a Regexp, a
    # Struct, a program's own class): the class loader's answer for a class
    # the load does not permit, and NotImplementedError for one it does,
    # since this package builds only Strings, numbers, Symbols, true, false,
    # nil, Arrays and Hashes.
    def build_class(name)
      raise DisallowedClass.new("load", name) unless permits?(name)
      raise NotImplementedError, "loading a #{name} from YAML is not supported in Spinel's psych"
    end

    def tokenize(string)
      return nil if string.empty?
      if string.match?(STRINGISH_RE) || string.include?("\n")
        return string if string.length > 5
        if string.match?(/^[^ytonf~]/i)
          string
        elsif string == "~" || string.match?(/^null$/i)
          nil
        elsif string.match?(/^(yes|true|on)$/i)
          true
        elsif string.match?(/^(no|false|off)$/i)
          false
        else
          string
        end
      elsif string.match?(TIME_RE)
        return :sp_time if @for_dump
        raise DisallowedClass.new("load", "Time") unless permits?("Time")
        raise NotImplementedError, "YAML timestamps (Time) are not supported in Spinel's psych"
      elsif string.match?(DATE_RE)
        return :sp_date if @for_dump
        raise DisallowedClass.new("load", "Date") unless permits?("Date")
        raise NotImplementedError, "YAML dates (Date) are not supported in Spinel's psych"
      elsif string.match?(/^\+?\.inf$/i)
        Float::INFINITY
      elsif string.match?(/^-\.inf$/i)
        -Float::INFINITY
      elsif string.match?(/^\.nan$/i)
        Float::NAN
      elsif @parse_symbols && string.match?(/^:./)
        if string =~ /^:(["'])(.*)\1/
          symbolize($2.sub(/^:/, ""))
        else
          symbolize(string.sub(/^:/, ""))
        end
      elsif string.match?(SEXAGESIMAL_INT_RE)
        i = 0
        parts = string.split(":")
        parts.each_with_index { |n, e| i += n.to_i * 60**(e - 2).abs }
        i
      elsif string.match?(SEXAGESIMAL_FLOAT_RE)
        f = 0.0
        parts = string.split(":")
        parts.each_with_index { |n, e| f += n.to_f * 60**(e - 2).abs }
        f
      elsif string.match?(FLOAT_RE)
        if string.match?(/\A[-+]?\.\Z/)
          string
        else
          Float(string.delete(",_").gsub(/\.([Ee]|$)/, '\1'))
        end
      elsif string.match?(@strict_integer ? INTEGER_STRICT_RE : INTEGER_LEGACY_RE)
        Integer(string.delete(",_"))
      else
        string
      end
    end
  end

  # One document, read straight into Ruby values. The scanner works on bytes;
  # `@ls` is the offset where the current line starts, so a column is
  # `@i - @ls`, and lines are counted for Psych::SyntaxError's position.
  class SpParser
    def initialize(src, file, resolver, aliases, symbolize_names)
      @kept = nil
      src = normalize_breaks(src)
      @s = src
      @n = src.bytesize
      @i = 0
      @ls = 0
      @line = 0
      @file = file
      @res = resolver
      @aliases = aliases
      @symbolize_names = symbolize_names
      @anchors = {}
    end

    # The line breaks, as libyaml's YAML 1.1 reading has them: CR LF, CR,
    # LF and U+0085 are one newline each, and U+2028 and U+2029 are breaks
    # too, kept as themselves where a newline would be folded or copied.
    # The reader tests one byte for a break, so the source is rewritten:
    # every newline is a LF, and a CR stands for a U+2028 or a U+2029, which
    # @kept names by the CR's byte offset (break_text).
    def normalize_breaks(src)
      unless src.include?("\r") || src.include?("\u0085") || src.include?("\u2028") || src.include?("\u2029")
        return src
      end
      s = src.gsub("\r\n", "\n").gsub("\r", "\n").gsub("\u0085", "\n")
      return s unless s.include?("\u2028") || s.include?("\u2029")
      out = +""
      kept = {}
      s.each_char do |ch|
        if ch == "\u2028" || ch == "\u2029"
          kept[out.bytesize] = ch
          out << "\r"
        else
          out << ch
        end
      end
      @kept = kept
      out
    end

    # The text of the break at the cursor: a newline, or the U+2028 or
    # U+2029 a CR stands for.
    def break_text
      return "\n" unless byte == 13
      k = @kept
      t = k.nil? ? nil : k[@i]
      t.nil? ? "\n" : t
    end

    # What a run of breaks inside a flow scalar becomes: a lone newline
    # folds to a space, more newlines keep all but the first, and a first
    # break that is U+2028 or U+2029 is kept with the rest.
    def fold_breaks(lead, trail)
      return lead + trail unless lead == "\n"
      trail.empty? ? " " : trail
    end

    # ---- the cursor ----

    def byte(o = 0)
      j = @i + o
      j < @n ? @s.getbyte(j) : 0
    end

    def col
      @i - @ls
    end

    def eof?
      @i >= @n
    end

    def blank_at?(o)
      b = byte(o)
      b == 32 || b == 9 || b == 10 || b == 13 || b == 0
    end

    def eol?
      b = byte
      b == 10 || b == 13 || b == 0 || (b == 35 && (@i == 0 || ws_byte?(@s.getbyte(@i - 1))))
    end

    def ws_byte?(b)
      b == 32 || b == 9 || b == 10 || b == 13
    end

    def newline
      @i += 1  # one break: one byte, after normalize_breaks
      @ls = @i
      @line += 1
    end

    def skip_space
      @i += 1 while byte == 32 || byte == 9
    end

    def skip_comment
      @i += 1 while !eof? && byte != 10 && byte != 13 if byte == 35
    end

    # Spaces, comments and line breaks up to the next content (or the end).
    def skip_to_content
      loop do
        if @i == @ls
          # a line's indentation is spaces: libyaml skips a tab only after
          # the line's first token, so one here cannot start anything
          @i += 1 while byte == 32
          if byte == 9
            error("found character that cannot start any token", "while scanning for the next token")
          end
        else
          skip_space
        end
        skip_comment
        if byte == 10 || byte == 13
          newline
        else
          return
        end
      end
    end

    def error(problem, context = nil, line = @line, column = col)
      raise SyntaxError.new(@file, line + 1, column + 1, @i, problem, context)
    end

    def doc_marker?
      return false unless col == 0
      three = @s.byteslice(@i, 3)
      (three == "---" || three == "...") && blank_at?(3)
    end

    def seq_entry?
      byte == 45 && blank_at?(1)
    end

    # ---- the document ----

    def parse_stream
      skip_to_content
      while byte == 37 && col == 0  # %YAML / %TAG directives
        @i += 1 until eof? || byte == 10 || byte == 13
        skip_to_content
      end
      if col == 0 && @s.byteslice(@i, 3) == "---" && blank_at?(3)
        @i += 3
        skip_space
        if eol?
          skip_to_content
          value = (eof? || doc_marker?) ? nil : parse_block_node(-1)
        else
          value = parse_inline_value(-1)
        end
        return [true, finish_document(value)]
      end
      return [false, nil] if eof?
      if col == 0 && @s.byteslice(@i, 3) == "..." && blank_at?(3)
        return [false, nil]
      end
      [true, finish_document(parse_block_node(-1))]
    end

    def finish_document(value)
      skip_to_content
      return value if eof? || doc_marker?
      error("did not find expected <document start>")
    end

    # ---- block nodes ----

    # The node starting at the cursor (its first content), inside a parent at
    # column `parent`. A node at or left of that column is empty.
    def parse_block_node(parent)
      return nil if eof? || doc_marker?
      c = col
      return nil if c <= parent
      b = byte
      if b == 38 || b == 33  # & anchor, ! tag
        return parse_properties(parent, true)
      end
      return parse_alias if b == 42
      return parse_block_sequence(c) if seq_entry?
      return parse_block_mapping(c) if mapping_key_here?
      parse_scalar_or_flow(parent, false)
    end

    # The value written on a key's or an entry's own line.
    def parse_inline_value(parent)
      b = byte
      return parse_properties(parent, false) if b == 38 || b == 33
      return parse_alias if b == 42
      error("block sequence entries are not allowed in this context") if seq_entry?
      parse_scalar_or_flow(parent, true)
    end

    def parse_scalar_or_flow(parent, inline)
      b = byte
      if b == 91 || b == 123  # [ {
        v = parse_flow_node(0)
        expect_line_end
        return v
      end
      if b == 124 || b == 62  # | >
        return parse_block_scalar(parent)
      end
      if b == 34 || b == 39
        v = parse_quoted(parent)
        if byte == 58 && blank_at?(1)
          error("mapping values are not allowed in this context") if inline
        end
        expect_line_end
        return v
      end
      text = parse_plain(parent, inline)
      resolve_plain(text)
    end

    def expect_line_end
      skip_space
      if byte == 58 && blank_at?(1)
        error("mapping values are not allowed in this context")
      end
      error("did not find expected key") unless eol?
    end

    def parse_properties(parent, block)
      pstart = @i
      pcol = col
      anchor = nil
      tag = nil
      while byte == 38 || byte == 33
        start = @i
        skip_property(true)
        word = @s.byteslice(start, @i - start)
        if word.start_with?("&")
          anchor = word[1..]
        else
          tag = word
        end
        skip_space
      end
      value =
        if eol?
          skip_to_content
          block_node_or_nil(parent)
        elsif block
          c = col
          if seq_entry?
            parse_block_sequence(c)
          elsif mapping_key_here?
            # properties on the line of an implicit key are the key's own
            # (`&a key: v`, `! '': v`): the mapping starts at them
            @i = pstart
            return parse_block_mapping(pcol)
          else
            parse_tagged(tag, parent, false)
          end
        else
          parse_tagged(tag, parent, true)
        end
      value = apply_tag(tag, value) if tag
      @anchors[anchor] = value if anchor
      value
    end

    def block_node_or_nil(parent)
      return nil if eof? || doc_marker?
      return parse_block_sequence(col, true) if col == parent && seq_entry? && parent >= 0
      parse_block_node(parent)
    end

    # A tagged scalar keeps its text for the tag to read.
    def parse_tagged(tag, parent, inline)
      return parse_scalar_or_flow(parent, inline) unless tag
      b = byte
      if b == 34 || b == 39
        v = parse_quoted(parent)
        expect_line_end
        return tagged_scalar(tag, v, true)
      end
      return parse_scalar_or_flow(parent, inline) if b == 91 || b == 123 || b == 124 || b == 62
      tagged_scalar(tag, parse_plain(parent, inline), false)
    end

    # A tagged scalar's value. A scalar with a tag is not "quoted" for Psych
    # (libyaml reports quoted_implicit only for an untagged one), so the tag
    # alone decides, quoted or plain: `! '3'` is 3, `! ''` is nil.
    def tagged_scalar(tag, text, quoted)
      [:sp_tagged_text, text]
    end

    # Psych::Visitors::ToRuby's reading of a tagged node: #deserialize for a
    # scalar, visit_Psych_Nodes_Sequence and _Mapping for a collection. Where
    # Psych would build an object of a class (a Regexp, a Struct, a program's
    # own class), the class loader decides first: an unpermitted class raises
    # DisallowedClass as in Psych, and a permitted one, which this package
    # cannot build, raises NotImplementedError.
    def apply_tag(tag, value)
      tag = expand_tag(tag)
      if value.is_a?(Hash)
        return tagged_mapping(tag, value)
      end
      if value.is_a?(Array) && !(value.size == 2 && value[0] == :sp_tagged_text)
        return tagged_sequence(tag, value)
      end
      text = value.is_a?(Array) ? value[1] : value.is_a?(String) ? value : ""
      tagged_text(tag, text)
    end

    # libyaml's tag handles: `!!x` is the YAML tag tag:yaml.org,2002:x and
    # `!<x>` is x verbatim; any other tag stays as written.
    def expand_tag(tag)
      return "tag:yaml.org,2002:#{tag[2..]}" if tag.start_with?("!!")
      return tag[2, tag.length - 3] if tag.start_with?("!<") && tag.end_with?(">")
      tag
    end

    def tagged_text(tag, text)
      case tag
      when "!binary", "tag:yaml.org,2002:binary"
        text.unpack1("m")
      when "tag:yaml.org,2002:str", "!str", "!ruby/string"
        text
      when "!ruby/object:BigDecimal"
        @res.build_class("BigDecimal")
      when "!ruby/object:DateTime"
        @res.build_class("DateTime")
      when "!ruby/encoding"
        raise NotImplementedError, "YAML tag !ruby/encoding (an Encoding) is not supported in Spinel's psych"
      when "!ruby/object:Complex"
        @res.build_class("Complex")
      when "!ruby/object:Rational"
        @res.build_class("Rational")
      when "!ruby/class", "!ruby/module"
        @res.build_class(text)
      when "tag:yaml.org,2002:float", "!float"
        to_float(@res.tokenize(text))
      when "!ruby/regexp"
        @res.build_class("Regexp")
      when "!ruby/range"
        @res.build_class("Range")
      else
        if tag =~ /\A!(?:str|ruby\/string):(.+)\z/
          @res.build_class($1)
        elsif tag.match?(/\A!ruby\/sym(bol)?:?/)
          @res.symbolize(text)
        else
          @res.tokenize(text)
        end
      end
    end

    # Kernel#Float on what the scalar resolved to, with its errors.
    def to_float(v)
      if v.is_a?(Integer)
        v.to_f
      elsif v.is_a?(Float)
        v
      elsif v.is_a?(String)
        Float(v)
      else
        raise TypeError, "can't convert #{v.inspect} into Float"
      end
    end

    def tagged_sequence(tag, list)
      if tag == "!omap" || tag == "tag:yaml.org,2002:omap"
        raise NotImplementedError, "YAML tag !!omap (a Psych::Omap) is not supported in Spinel's psych"
      end
      @res.build_class($1) if tag =~ /\A!(?:seq|ruby\/array):(.*)\z/
      list
    end

    def tagged_mapping(tag, h)
      if tag =~ /\A!ruby\/struct:?(.*)\z/
        @res.build_class($1.empty? ? "Struct" : $1)
      elsif tag =~ /\A!ruby\/data(?:-with-ivars)?(?::(.*))?\z/
        @res.build_class($1.nil? || $1.empty? ? "Data" : $1)
      elsif tag =~ /\A!ruby\/object:?(.*)\z/
        name = $1.empty? ? "Object" : $1
        return h if name == "Hash"
        @res.build_class(name)
      elsif tag =~ /\A!(?:str|ruby\/string)(?::(.*))?\z/ || tag == "tag:yaml.org,2002:str"
        @res.build_class($1) if $1 && !$1.empty?
        return nil if h.empty?
        # Psych sets the other keys as instance variables of the String, or
        # of nil when there is no "str" key
        raise FrozenError, "can't modify frozen NilClass: nil" unless h.key?("str")
        raise NotImplementedError, "a tagged String with instance variables is not supported in Spinel's psych" unless h.size == 1
        h["str"]
      elsif tag =~ /\A!ruby\/array:(.*)\z/
        @res.build_class($1)
      elsif tag == "!ruby/range"
        @res.build_class("Range")
      elsif tag =~ /\A!ruby\/exception:?(.*)\z/
        @res.build_class($1.empty? ? "Exception" : $1)
      elsif tag == "!set" || tag == "tag:yaml.org,2002:set"
        @res.build_class("Psych::Set")
      elsif tag =~ /\A!ruby\/hash-with-ivars(?::(.*))?\z/
        @res.build_class($1) if $1 && !$1.empty?
        raise NotImplementedError, "YAML tag !ruby/hash-with-ivars is not supported in Spinel's psych"
      elsif tag =~ /\A!(?:map|ruby\/hash):(.*)\z/
        @res.build_class($1)
      elsif tag == "!omap" || tag == "tag:yaml.org,2002:omap"
        @res.build_class("Psych::Omap")
      elsif tag =~ /\A!ruby\/marshalable:(.*)\z/
        @res.build_class($1)
      else
        h
      end
    end

    # One `&anchor` or `!tag` property: a verbatim `!<...>` tag runs to its
    # closing `>`, any other ends at a blank (in a flow also at `,`, `]`, `}`).
    def skip_property(flow)
      if byte == 33 && byte(1) == 60
        @i += 1 until byte == 62 || eof?
        @i += 1 unless eof?
      elsif flow
        @i += 1 until blank_at?(0) || byte == 44 || byte == 93 || byte == 125
      else
        @i += 1 until blank_at?(0)
      end
    end

    def parse_alias
      start = @i + 1
      @i += 1 until blank_at?(0) || byte == 44 || byte == 93 || byte == 125
      name = @s.byteslice(start, @i - start)
      raise AliasesNotEnabled.new unless @aliases
      raise AnchorNotDefined.new(name) unless @anchors.key?(name)
      @anchors[name]
    end

    # Is the content at the cursor a mapping key (`key:` then a blank)?
    def mapping_key_here?
      return true if byte == 63 && blank_at?(1)  # explicit `? key`
      j = @i
      b = byte
      if b == 34 || b == 39
        j = quoted_end(j)
        return false if j < 0
        j += 1
        j += 1 while j < @n && (@s.getbyte(j) == 32 || @s.getbyte(j) == 9)
        return j < @n && @s.getbyte(j) == 58 && blank_byte_at?(j + 1)
      end
      return false if b == 91 || b == 123 || b == 124 || b == 62 || b == 35
      while j < @n
        bj = @s.getbyte(j)
        return false if bj == 10 || bj == 13
        return false if bj == 35 && j > @i && ws_byte?(@s.getbyte(j - 1))
        return true if bj == 58 && blank_byte_at?(j + 1)
        j += 1
      end
      false
    end

    def blank_byte_at?(j)
      return true if j >= @n
      b = @s.getbyte(j)
      b == 32 || b == 9 || b == 10 || b == 13
    end

    # The offset of a quoted scalar's closing quote on its own line, or -1.
    def quoted_end(j)
      q = @s.getbyte(j)
      j += 1
      while j < @n
        b = @s.getbyte(j)
        return -1 if b == 10 || b == 13
        if q == 39 && b == 39
          return j unless j + 1 < @n && @s.getbyte(j + 1) == 39
          j += 2
          next
        end
        if q == 34 && b == 92
          j += 2
          next
        end
        return j if b == q
        j += 1
      end
      -1
    end

    def parse_block_mapping(indent)
      h = {}
      start_line = @line
      start_col = col
      loop do
        if byte == 63 && blank_at?(1)
          @i += 1
          skip_space
          key = eol? ? (skip_to_content; parse_block_node(indent)) : parse_inline_value(indent)
          skip_to_content
          if col == indent && byte == 58 && blank_at?(1)
            @i += 1
            skip_space
            # (an explicit key's value may be a block collection that starts
            # on the `:` line: `? a` / `: b: c`, `: - b`)
            value = eol? ? mapping_value(indent) : parse_block_node(indent)
          else
            value = nil
          end
          store(h, key, value, false)
        else
          key_tag = nil
          key_anchor = nil
          while byte == 33 || byte == 38
            start = @i
            skip_property(false)
            word = @s.byteslice(start, @i - start)
            word.start_with?("&") ? (key_anchor = word[1..]) : (key_tag = word)
            skip_space
          end
          key_is_text = byte == 34 || byte == 39
          key_text = key_is_text ? parse_quoted(indent) : parse_plain_key
          key =
            if key_tag
              apply_tag(key_tag, tagged_scalar(key_tag, key_text, key_is_text))
            elsif key_is_text
              key_text
            else
              resolve_plain(key_text)
            end
          key_is_text ||= !key_tag.nil?
          @anchors[key_anchor] = key if key_anchor
          skip_space
          error("could not find expected ':'", "while scanning a simple key") unless byte == 58
          @i += 1
          skip_space
          value = mapping_value(indent)
          store(h, key, value, !key_is_text)
        end
        skip_to_content
        break if eof? || doc_marker?
        c = col
        break if c < indent
        if c > indent
          error("did not find expected key", "while parsing a block mapping", start_line, start_col)
        end
        if seq_entry?
          error("did not find expected key", "while parsing a block mapping", start_line, start_col)
        end
      end
      h
    end

    def mapping_value(indent)
      if eol?
        skip_to_content
        return nil if eof? || doc_marker?
        return parse_block_sequence(indent, true) if col == indent && seq_entry?
        return parse_block_node(indent)
      end
      parse_inline_value(indent)
    end

    # A merge key (`<<: *defaults`) folds a mapping, or each of a list of
    # them, into this one, as Psych's ToRuby does; any other key is set.
    def store(h, key, value, plain_key)
      if plain_key && key == "<<"
        if value.is_a?(Hash)
          h.merge!(value)
          return
        end
        if value.is_a?(Array) && value.all? { |x| x.is_a?(Hash) }
          value.reverse_each { |x| h.merge!(x) }
          return
        end
      end
      key = key.to_sym if @symbolize_names && key.is_a?(String)
      h[key] = value
    end

    # `indentless`: the value of a key at the key's own column (`a:\n- 1`),
    # which the next key ends; any other sequence ends only by dedenting.
    def parse_block_sequence(indent, indentless = false)
      a = []
      start_line = @line
      start_col = col
      loop do
        @i += 1  # the `-`
        skip_space
        if eol?
          skip_to_content
          a << ((eof? || doc_marker? || col <= indent) ? nil : parse_block_node(indent))
        else
          a << parse_block_node(indent)
        end
        skip_to_content
        break if eof? || doc_marker?
        c = col
        break if c < indent
        if c == indent
          next if seq_entry?
          break if indentless
        end
        error("did not find expected '-' indicator", "while parsing a block collection", start_line, start_col)
      end
      a
    end

    # ---- plain scalars ----

    # A key's text: up to the `:` that ends it.
    def parse_plain_key
      start = @i
      loop do
        b = byte
        break if b == 10 || b == 13 || b == 0
        break if b == 58 && blank_at?(1)
        @i += 1
      end
      @s.byteslice(start, @i - start).rstrip
    end

    # A plain scalar in block context, folded over its continuation lines:
    # each further line indented past `parent` joins with a space, and an
    # empty line between two is a line break.
    def parse_plain(parent, inline)
      text = plain_line(inline)
      loop do
        save_i = @i
        save_ls = @ls
        save_line = @line
        skip_space
        break unless byte == 10 || byte == 13
        lead = break_text
        trail = +""
        newline
        skip_space
        while byte == 10 || byte == 13
          trail << break_text
          newline
          skip_space
        end
        if eof? || col <= parent || byte == 35 || doc_marker? || (parent < 0 && col == 0 && continuation_is_structure?)
          @i = save_i
          @ls = save_ls
          @line = save_line
          break
        end
        if parent >= 0 && continuation_is_structure? && mapping_key_here?
          # libyaml points at the `:` that makes the line a key
          @i += 1 until byte == 58 && blank_at?(1)
          error("mapping values are not allowed in this context")
        end
        text += fold_breaks(lead, trail)
        text += plain_line(inline)
      end
      text
    end

    # (a `? ` on a plain scalar's continuation line is the scalar's text, as
    # libyaml reads it: only a `key:` or a `- ` there is structure)
    def continuation_is_structure?
      seq_entry? || (mapping_key_here? && !(byte == 63 && blank_at?(1)))
    end

    def plain_line(inline)
      start = @i
      loop do
        b = byte
        break if b == 10 || b == 13 || b == 0
        break if b == 35 && @i > start && ws_byte?(@s.getbyte(@i - 1))
        if b == 58 && blank_at?(1)
          error("mapping values are not allowed in this context") if inline
          break
        end
        @i += 1
      end
      @s.byteslice(start, @i - start).rstrip
    end

    def resolve_plain(text)
      @res.tokenize(text)
    end

    # ---- quoted scalars ----

    def parse_quoted(parent)
      q = byte
      start_line = @line
      start_col = col
      @i += 1
      out = +""
      loop do
        if eof?
          error("found unexpected end of stream", "while scanning a quoted scalar", start_line, start_col)
        end
        b = byte
        if q == 39 && b == 39
          if byte(1) == 39
            out << "'"
            @i += 2
            next
          end
          @i += 1
          break
        end
        if q == 34 && b == 34
          @i += 1
          break
        end
        if q == 34 && b == 92
          if byte(1) == 10 || byte(1) == 13
            @i += 1
            newline
            skip_space
            next
          end
          out << escape
          next
        end
        if b == 10 || b == 13 || b == 32 || b == 9
          fold_quoted(out)
          next
        end
        out << read_char
      end
      out
    end

    # Whitespace inside a quoted scalar: trailing spaces before a break are
    # dropped, a single break becomes a space and each further one a break.
    def fold_quoted(out)
      ws_start = @i
      @i += 1 while byte == 32 || byte == 9
      unless byte == 10 || byte == 13
        out << @s.byteslice(ws_start, @i - ws_start)
        return
      end
      lead = break_text
      trail = +""
      newline
      skip_space
      while byte == 10 || byte == 13
        trail << break_text
        newline
        skip_space
      end
      out << fold_breaks(lead, trail)
    end

    def read_char
      b = byte
      len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4
      c = @s.byteslice(@i, len)
      @i += len
      c
    end

    ESCAPES = { "0" => "\0", "a" => "\a", "b" => "\b", "t" => "\t", "\t" => "\t", "n" => "\n",
                "v" => "\v", "f" => "\f", "r" => "\r", "e" => "\e", " " => " ", "\"" => "\"",
                "/" => "/", "\\" => "\\", "N" => "\u0085", "_" => "\u00A0", "L" => "\u2028",
                "P" => "\u2029" }

    def escape
      @i += 1
      c = @s.byteslice(@i, 1)
      @i += 1
      return ESCAPES[c] if ESCAPES.key?(c)
      width = c == "x" ? 2 : c == "u" ? 4 : c == "U" ? 8 : 0
      error("found unknown escape character", "while parsing a quoted scalar") if width == 0
      hex = @s.byteslice(@i, width)
      error("did not find expected hexdecimal number", "while parsing a quoted scalar") unless hex.match?(/\A\h+\z/) && hex.bytesize == width
      @i += width
      [hex.to_i(16)].pack("U")
    end

    # ---- block scalars ----

    def parse_block_scalar(parent)
      folded = byte == 62
      start_line = @line
      start_col = col
      @i += 1
      chomp = 0
      explicit = 0
      2.times do
        b = byte
        if b == 43
          chomp = 1
          @i += 1
        elsif b == 45
          chomp = -1
          @i += 1
        elsif b >= 49 && b <= 57
          explicit = b - 48
          @i += 1
        end
      end
      skip_space
      skip_comment
      unless eof? || byte == 10 || byte == 13
        error("did not find expected comment or line break", "while scanning a block scalar", start_line, start_col)
      end
      newline unless eof?
      base = parent < 0 ? 0 : parent
      indent = explicit > 0 ? (parent < 0 ? explicit : parent + explicit) : 0
      trailing = +""
      # leading empty lines, and the indentation the first content line sets
      max_indent = 0
      loop do
        @i += 1 while byte == 32 && (indent == 0 || col < indent)
        max_indent = col if col > max_indent
        if byte == 10 || byte == 13
          trailing << break_text
          newline
          next
        end
        break
      end
      if indent == 0
        indent = max_indent
        indent = base + 1 if indent <= base
        indent = 1 if indent < 1
      end
      out = +""
      leading_break = +""
      leading_blank = false
      while col == indent && !eof?
        trailing_blank = byte == 32 || byte == 9
        if folded && leading_break == "\n" && !leading_blank && !trailing_blank
          out << " " if trailing.empty?
          leading_break = +""
        else
          out << leading_break
          leading_break = +""
        end
        out << trailing
        trailing = +""
        leading_blank = byte == 32 || byte == 9
        start = @i
        @i += 1 until eof? || byte == 10 || byte == 13
        out << @s.byteslice(start, @i - start)
        break if eof?
        leading_break = +break_text
        newline
        loop do
          @i += 1 while byte == 32 && col < indent
          if byte == 10 || byte == 13
            trailing << break_text
            newline
            next
          end
          break
        end
      end
      out << leading_break if chomp != -1
      out << trailing if chomp == 1
      # what follows starts at the line's first column, not inside it
      @i = @ls if col <= indent && !eof?
      out
    end

    # ---- flow collections ----

    def flow_space
      loop do
        skip_space
        skip_comment
        if byte == 10 || byte == 13
          newline
        else
          return
        end
      end
    end

    # The input ended where a node had to be. libyaml marks the end of the
    # stream on the line after the last one when the text has no final break.
    def node_content_error
      line = @line
      line += 1 if @n > 0 && @s.getbyte(@n - 1) != 10
      error("did not find expected node content", "while parsing a flow node", line, 0)
    end

    def parse_flow_node(depth)
      flow_space
      node_content_error if eof?
      b = byte
      if b == 91
        return parse_flow_sequence(depth)
      end
      if b == 123
        return parse_flow_mapping(depth)
      end
      if b == 38 || b == 33
        anchor = nil
        tag = nil
        while byte == 38 || byte == 33
          start = @i
          skip_property(true)
          word = @s.byteslice(start, @i - start)
          word.start_with?("&") ? (anchor = word[1..]) : (tag = word)
          flow_space
        end
        v =
          if tag.nil?
            parse_flow_node(depth)
          elsif byte == 91 || byte == 123 || byte == 34 || byte == 39
            apply_tag(tag, parse_flow_node(depth))
          else
            apply_tag(tag, [:sp_tagged_text, flow_scalar_text])
          end
        @anchors[anchor] = v if anchor
        return v
      end
      return parse_alias if b == 42
      return parse_quoted(0) if b == 34 || b == 39
      resolve_plain(flow_scalar_text)
    end

    def flow_scalar_text
      start = @i
      loop do
        b = byte
        break if b == 0 || b == 44 || b == 91 || b == 93 || b == 123 || b == 125 || b == 10 || b == 13
        break if b == 58 && (blank_at?(1) || byte(1) == 44 || byte(1) == 93 || byte(1) == 125)
        break if b == 35 && @i > start && ws_byte?(@s.getbyte(@i - 1))
        @i += 1
      end
      @s.byteslice(start, @i - start).strip
    end

    def parse_flow_sequence(depth)
      line = @line
      c = col
      @i += 1
      a = []
      loop do
        flow_space
        if byte == 93
          @i += 1
          return a
        end
        node_content_error if eof?
        item = parse_flow_node(depth + 1)
        flow_space
        if byte == 58 && (blank_at?(1) || byte(1) == 44 || byte(1) == 93)
          @i += 1
          flow_space
          v = (byte == 44 || byte == 93) ? nil : parse_flow_node(depth + 1)
          pair = {}
          store(pair, item, v, true)
          item = pair
          flow_space
        end
        a << item
        if byte == 44
          @i += 1
        elsif byte != 93
          error("did not find expected ',' or ']'", "while parsing a flow sequence", line, c)
        end
      end
    end

    def parse_flow_mapping(depth)
      line = @line
      c = col
      @i += 1
      h = {}
      loop do
        flow_space
        if byte == 125
          @i += 1
          return h
        end
        node_content_error if eof?
        @i += 1 if byte == 63 && blank_at?(1)
        flow_space
        plain_key = !(byte == 34 || byte == 39)
        key = parse_flow_node(depth + 1)
        flow_space
        value = nil
        if byte == 58
          @i += 1
          flow_space
          value = (byte == 44 || byte == 125) ? nil : parse_flow_node(depth + 1)
          flow_space
        end
        store(h, key, value, plain_key)
        if byte == 44
          @i += 1
        elsif byte != 125
          error("did not find expected ',' or '}'", "while parsing a flow mapping", line, c)
        end
      end
    end
  end

  # ---- loading ----

  def self.safe_load(yaml, permitted_classes: [], permitted_symbols: [], aliases: false, filename: nil,
                     fallback: nil, symbolize_names: false, freeze: false, strict_integer: false, parse_symbols: true)
    resolver = SpResolver.new(permitted_classes, permitted_symbols, strict_integer, parse_symbols)
    sp_load(yaml, resolver, aliases, filename, fallback, symbolize_names, freeze)
  end

  def self.sp_load(yaml, resolver, aliases, filename, fallback, symbolize_names, freeze)
    raise NotImplementedError, "Psych.safe_load(freeze: true) is not supported in Spinel's psych" if freeze
    src = yaml.is_a?(String) ? yaml : yaml.read
    src = src.byteslice(3, src.bytesize - 3) if src.start_with?("﻿")
    found, value = SpParser.new(src, filename, resolver, aliases, symbolize_names).parse_stream
    found ? value : fallback
  end

  # The tag tables a program registers its classes in (activesupport's
  # time_with_zone.rb, actionpack's Parameters): Psych's load_tags maps a
  # tag to the class name a load builds, dump_tags a class to the tag a
  # dump writes. Building or dumping such an object is not done here, so
  # the tables are kept as written and answer what was written.
  @load_tags = {}
  @dump_tags = {}
  def self.load_tags = @load_tags
  def self.load_tags=(tags)
    @load_tags = tags
  end
  def self.dump_tags = @dump_tags
  def self.dump_tags=(tags)
    @dump_tags = tags
  end

  def self.load(yaml, permitted_classes: [Symbol], permitted_symbols: [], aliases: false, filename: nil,
                fallback: nil, symbolize_names: false, freeze: false, strict_integer: false, parse_symbols: true)
    safe_load(yaml, permitted_classes: permitted_classes, permitted_symbols: permitted_symbols, aliases: aliases,
                    filename: filename, fallback: fallback, symbolize_names: symbolize_names, freeze: freeze,
                    strict_integer: strict_integer, parse_symbols: parse_symbols)
  end

  def self.unsafe_load(yaml, filename: nil, fallback: false, symbolize_names: false, freeze: false, strict_integer: false)
    resolver = SpResolver.new([], [], strict_integer, true, false, true)
    sp_load(yaml, resolver, true, filename, fallback, symbolize_names, freeze)
  end

  def self.safe_load_file(filename, permitted_classes: [], permitted_symbols: [], aliases: false, fallback: nil,
                          symbolize_names: false, freeze: false, strict_integer: false, parse_symbols: true)
    safe_load(File.read(filename), permitted_classes: permitted_classes, permitted_symbols: permitted_symbols,
                                   aliases: aliases, filename: filename, fallback: fallback,
                                   symbolize_names: symbolize_names, freeze: freeze,
                                   strict_integer: strict_integer, parse_symbols: parse_symbols)
  end

  def self.load_file(filename, permitted_classes: [Symbol], permitted_symbols: [], aliases: false, fallback: nil,
                     symbolize_names: false, freeze: false, strict_integer: false, parse_symbols: true)
    safe_load(File.read(filename), permitted_classes: permitted_classes, permitted_symbols: permitted_symbols,
                                   aliases: aliases, filename: filename, fallback: fallback,
                                   symbolize_names: symbolize_names, freeze: freeze,
                                   strict_integer: strict_integer, parse_symbols: parse_symbols)
  end

  def self.unsafe_load_file(filename, fallback: false, symbolize_names: false, freeze: false, strict_integer: false)
    unsafe_load(File.read(filename), filename: filename, fallback: fallback,
                                     symbolize_names: symbolize_names, freeze: freeze, strict_integer: strict_integer)
  end

  # ---- dumping ----

  # libyaml's emitter, the block-style half that Psych.dump drives: scalars
  # take the style Psych asks for unless their characters rule it out
  # (yaml_emitter_analyze_scalar / select_scalar_style), lines fold at 80
  # columns, and collections indent two spaces, a sequence under a key at
  # the key's own column.
  class SpEmitter
    BEST_WIDTH = 80

    def initialize(resolver)
      @out = +""
      @column = 0
      @whitespace = true
      @indention = true
      @indent = -1
      @indents = []
      @res = resolver
    end

    def result
      @out
    end

    def put(str)
      @out << str
      @column += str.length
    end

    def put_break
      @out << "\n"
      @column = 0
    end

    def write_indent
      ind = @indent >= 0 ? @indent : 0
      put_break if !@indention || @column > ind || (@column == ind && !@whitespace)
      put(" " * (ind - @column)) if @column < ind
      @whitespace = true
      @indention = true
    end

    def write_indicator(ind, need_whitespace, is_whitespace, is_indention)
      put(" ") if need_whitespace && !@whitespace
      put(ind)
      @whitespace = is_whitespace
      @indention = @indention && is_indention
    end

    def increase_indent(flow, indentless)
      @indents << @indent
      if @indent < 0
        @indent = flow ? 2 : 0
      elsif !indentless
        @indent += 2
      end
    end

    def decrease_indent
      @indent = @indents.pop
    end

    def document(obj)
      write_indicator("---", true, false, false)
      node(obj, root: true)
      write_indent
      @out
    end

    def node(obj, root: false, sequence: false, mapping: false, simple_key: false)
      if obj.is_a?(Hash)
        if obj.empty?
          flow_empty("{", "}")
        else
          block_mapping(obj, mapping)
        end
      elsif obj.is_a?(Array)
        if obj.empty?
          flow_empty("[", "]")
        else
          block_sequence(obj, mapping)
        end
      else
        scalar_node(obj, simple_key)
      end
    end

    def flow_empty(open, close)
      write_indicator(open, true, true, false)
      write_indicator(close, false, false, false)
    end

    def block_sequence(arr, in_mapping)
      increase_indent(false, in_mapping && !@indention)
      arr.each do |item|
        write_indent
        write_indicator("-", true, false, true)
        node(item, sequence: true)
      end
      decrease_indent
    end

    def block_mapping(hash, _in_mapping)
      increase_indent(false, false)
      hash.each do |k, v|
        write_indent
        if k.is_a?(Hash) || k.is_a?(Array)
          raise NotImplementedError, "a collection as a mapping key is not supported in Spinel's psych dump"
        end
        if simple_key?(k)
          node(k, mapping: true, simple_key: true)
          write_indicator(":", false, false, false)
        else
          # libyaml's complex key: `? ` before it and `: ` on a line of its own
          write_indicator("?", true, false, true)
          node(k, mapping: true)
          write_indent
          write_indicator(":", true, false, true)
        end
        node(v, mapping: true)
      end
      decrease_indent
    end

    # yaml_emitter_check_simple_key for a scalar: one line, 128 characters
    # at most (the `:sym` and `! ''` spellings counted as written).
    def simple_key?(k)
      text = k.is_a?(String) ? k : k.is_a?(Symbol) ? ":#{k}" : k.nil? ? "! ''" : k.to_s
      text.length <= 128 && !text.include?("\n")
    end

    # What Psych's YAMLTree asks for (value, plain/quoted implicitness,
    # style, tag), per class.
    def scalar_node(obj, simple_key)
      if obj.nil?
        if simple_key
          write_indicator("!", true, false, false)
          emit_scalar("", "'", simple_key)
        else
          emit_scalar("", "plain", simple_key)
        end
      elsif obj == true || obj == false || obj.is_a?(Integer)
        emit_scalar(obj.to_s, "plain", simple_key)
      elsif obj.is_a?(Float)
        text = obj.nan? ? ".nan" : obj.infinite? ? (obj > 0 ? ".inf" : "-.inf") : obj.to_s
        emit_scalar(text, "plain", simple_key)
      elsif obj.is_a?(Symbol)
        raise NotImplementedError, "an empty Symbol is not supported in Spinel's psych dump" if obj.to_s.empty?
        emit_scalar(":#{obj}", "plain", simple_key)
      elsif obj.is_a?(String)
        string_node(obj, simple_key)
      else
        raise NotImplementedError, "Psych.dump of a #{obj.class} is not supported in Spinel's psych"
      end
    end

    def string_node(o, simple_key)
      style = "plain"
      if o.match?(/\n(?!\Z)/)
        style = "|"
      elsif o == "<<"
        write_indicator("!!str", true, false, false)
        emit_scalar(o, "'", simple_key)
        return
      elsif o == "y" || o == "Y" || o == "n" || o == "N"
        style = "\""
      elsif o.match?(/^[^[:word:]][^"]*$/)
        style = "\""
      elsif !@res.tokenize(o).is_a?(String) || o.match?(/\A0[0-7]*[89]/)
        style = "'"
      end
      emit_scalar(o, style, simple_key)
    end

    def emit_scalar(value, style, simple_key)
      a = analyze(value)
      style = "\"" if simple_key && a[:multiline]
      if style == "plain"
        style = "'" unless a[:block_plain]
        style = "'" if value.empty? && simple_key
      end
      style = "\"" if style == "'" && !a[:single]
      style = "\"" if style == "|" && (!a[:block] || simple_key)
      increase_indent(true, false)
      case style
      when "plain" then write_plain(value, !simple_key)
      when "'" then write_single(value, !simple_key)
      when "\"" then write_double(value, !simple_key)
      else write_literal(value)
      end
      decrease_indent
    end

    def space_char?(c)
      c == " "
    end

    def break_char?(c)
      c == "\n" || c == "\r" || c == "\u0085" || c == "\u2028" || c == "\u2029"
    end

    def blankz?(c)
      c.nil? || c == " " || c == "\t" || break_char?(c)
    end

    def printable?(c)
      o = c.ord
      o == 0x0A || (o >= 0x20 && o <= 0x7E) || o == 0x85 || (o >= 0xA0 && o <= 0xD7FF) ||
        (o >= 0xE000 && o <= 0xFFFD && o != 0xFEFF) || (o >= 0x10000 && o <= 0x10FFFF)
    end

    # yaml_emitter_analyze_scalar
    def analyze(value)
      if value.empty?
        return { multiline: false, block_plain: true, single: true, block: false }
      end
      chars = value.chars
      block_ind = false
      special = false
      line_breaks = false
      leading_space = leading_break = trailing_space = trailing_break = false
      break_space = space_break = previous_space = previous_break = false
      if value.start_with?("---") || value.start_with?("...")
        block_ind = true if blankz?(chars[3])
      end
      preceded_ws = true
      last = chars.size - 1
      chars.each_with_index do |c, i|
        followed_ws = blankz?(chars[i + 1])
        if i == 0
          block_ind = true if "#,[]{}&*!|>'\"%@`".include?(c)
          block_ind = true if (c == "?" || c == ":") && followed_ws
          block_ind = true if c == "-" && followed_ws
        else
          block_ind = true if c == ":" && followed_ws
          block_ind = true if c == "#" && preceded_ws
        end
        special = true unless printable?(c)
        line_breaks = true if break_char?(c)
        if space_char?(c)
          leading_space = true if i == 0
          trailing_space = true if i == last
          break_space = true if previous_break
          previous_space = true
          previous_break = false
        elsif break_char?(c)
          leading_break = true if i == 0
          trailing_break = true if i == last
          space_break = true if previous_space
          previous_break = true
          previous_space = false
        else
          previous_space = false
          previous_break = false
        end
        preceded_ws = blankz?(c)
      end
      block_plain = true
      single = true
      block = true
      block_plain = false if leading_space || leading_break || trailing_space || trailing_break
      block = false if trailing_space
      if break_space
        block_plain = false
        single = false
      end
      if space_break || special
        block_plain = false
        single = false
        block = false
      end
      block_plain = false if line_breaks || block_ind
      { multiline: line_breaks, block_plain: block_plain, single: single, block: block }
    end

    def write_plain(value, allow_breaks)
      put(" ") if !@whitespace && !value.empty?
      spaces = false
      chars = value.chars
      chars.each_with_index do |c, i|
        if space_char?(c)
          if allow_breaks && !spaces && @column > BEST_WIDTH && chars[i + 1] != " "
            write_indent
          else
            put(c)
          end
          spaces = true
        else
          put(c)
          @indention = false
          spaces = false
        end
      end
      @whitespace = false
      @indention = false
    end

    def write_single(value, allow_breaks)
      write_indicator("'", true, false, false)
      spaces = false
      breaks = false
      chars = value.chars
      last = chars.size - 1
      chars.each_with_index do |c, i|
        if space_char?(c)
          if allow_breaks && !spaces && @column > BEST_WIDTH && i != 0 && i != last && chars[i + 1] != " "
            write_indent
          else
            put(c)
          end
          spaces = true
        elsif break_char?(c)
          put_break if !breaks && c == "\n"
          put_break
          @indention = true
          breaks = true
        else
          write_indent if breaks
          put("'") if c == "'"
          put(c)
          @indention = false
          spaces = false
          breaks = false
        end
      end
      write_indent if breaks
      write_indicator("'", false, false, false)
    end

    DOUBLE_ESCAPES = { "\0" => "0", "\a" => "a", "\b" => "b", "\t" => "t", "\n" => "n", "\v" => "v",
                       "\f" => "f", "\r" => "r", "\e" => "e", "\"" => "\"", "\\" => "\\",
                       "\u0085" => "N", "\u00A0" => "_", "\u2028" => "L", "\u2029" => "P" }

    def write_double(value, allow_breaks)
      write_indicator("\"", true, false, false)
      spaces = false
      chars = value.chars
      last = chars.size - 1
      chars.each_with_index do |c, i|
        if !printable?(c) || c == "\uFEFF" || break_char?(c) || c == "\"" || c == "\\"
          put("\\")
          if DOUBLE_ESCAPES.key?(c)
            put(DOUBLE_ESCAPES[c])
          else
            o = c.ord
            put(o <= 0xFF ? format("x%02X", o) : o <= 0xFFFF ? format("u%04X", o) : format("U%08X", o))
          end
          spaces = false
        elsif space_char?(c)
          if allow_breaks && !spaces && @column > BEST_WIDTH && i != 0 && i != last
            write_indent
            put("\\") if chars[i + 1] == " "
          else
            put(c)
          end
          spaces = true
        else
          put(c)
          spaces = false
        end
      end
      write_indicator("\"", false, false, false)
    end

    def write_literal(value)
      write_indicator("|", true, false, false)
      chars = value.chars
      hint = +""
      hint << "2" if space_char?(chars[0]) || break_char?(chars[0])
      if !break_char?(chars[-1])
        hint << "-"
      elsif chars.size == 1 || break_char?(chars[-2])
        hint << "+"
      end
      put(hint) unless hint.empty?
      put_break
      @indention = true
      @whitespace = true
      breaks = true
      chars.each do |c|
        if break_char?(c)
          put_break
          @indention = true
          breaks = true
        else
          write_indent if breaks
          put(c)
          @indention = false
          breaks = false
        end
      end
    end
  end

  def self.dump(o, io = nil, options = {})
    options = io if io.is_a?(Hash)
    resolver = SpResolver.new(["Symbol"], [], false, true, true)
    text = SpEmitter.new(resolver).document(o)
    if io && !io.is_a?(Hash)
      io.write(text)
      return io
    end
    text
  end

  def self.safe_dump(o, io = nil, permitted_classes: [], permitted_symbols: [], **options)
    sp_check_dump(o, permitted_classes.map { |c| c.to_s }, permitted_symbols.map { |s| s.to_s })
    dump(o, io, options)
  end

  def self.sp_check_dump(o, classes, symbols)
    if o.is_a?(Hash)
      o.each do |k, v|
        sp_check_dump(k, classes, symbols)
        sp_check_dump(v, classes, symbols)
      end
    elsif o.is_a?(Array)
      o.each { |x| sp_check_dump(x, classes, symbols) }
    elsif o.is_a?(Symbol)
      unless classes.include?("Symbol") || symbols.include?(o.to_s)
        raise DisallowedClass.new("dump", "Symbol(#{o.inspect})")
      end
    end
  end
end

class Object
  def to_yaml(options = {})
    Psych.dump(self, options)
  end
end
