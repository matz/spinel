# Spinel bundled `cgi` -- the escape and unescape surface, in plain Ruby.
#
# WHAT `require "cgi"` DEFINES IS NOT THE CGI CLASS, and has not been
# since CRuby split the library: on Ruby 4.0 the feature answers with
# `escape`, `unescape`, `escapeHTML`, `unescapeHTML`,
# `escapeURIComponent`, `unescapeURIComponent`, their snake_case
# aliases and `h`, and nothing else. The request/response object, and
# `CGI.parse` with it, moved to `cgi/core`. So this package is not a
# subset of the library chosen to fit a compiler -- it IS the feature,
# minus the two methods named below.
#
# `escapeElement` and `unescapeElement` are the two left out. They take
# a list of tag names, build a pattern from it and escape everything
# but those elements; the pattern is the method, and writing it without
# one would be a different function wearing the name. Nothing here
# needs them, and a caller that does is better served by the refusal
# than by an approximation.
#
# EVERY RULE BELOW IS CRuby'S, READ OFF CRuby. The unreserved set is
# the 66 bytes `CGI.escape` returns unchanged (`-.0-9A-Z_a-z~`, which
# `escapeURIComponent` keeps too -- the two differ only in the space);
# a percent escape is uppercase and per BYTE, so a multi-byte character
# becomes several; an unescape reads either case and leaves a truncated
# or non-hex `%` alone; and an entity that is not one of the five
# names, or whose number is not a codepoint, is passed through as it
# was written.
class CGI
  # The bytes CGI.escape returns unchanged. A 256-entry table would
  # read the same and cost a load-time array; the ranges are four
  # comparisons.
  def self.unreserved_byte?(b)
    return true if b >= 0x30 && b <= 0x39   # 0-9
    return true if b >= 0x41 && b <= 0x5A   # A-Z
    return true if b >= 0x61 && b <= 0x7A   # a-z
    b == 0x2D || b == 0x2E || b == 0x5F || b == 0x7E   # - . _ ~
  end

  HEX_DIGITS = "0123456789ABCDEF"

  # The two escapes differ in one byte: form encoding writes a space as
  # "+", the URI-component one percent-escapes it like anything else.
  def self.percent_escape(string, space_as_plus)
    out = +""
    i = 0
    n = string.bytesize
    while i < n
      b = string.getbyte(i)
      if unreserved_byte?(b)
        out << b.chr
      elsif b == 0x20 && space_as_plus
        out << "+"
      else
        out << "%" << HEX_DIGITS[b >> 4] << HEX_DIGITS[b & 0x0F]
      end
      i += 1
    end
    out
  end

  def self.escape(string)
    percent_escape(string, true)
  end

  def self.escapeURIComponent(string)
    percent_escape(string, false)
  end

  def self.escape_uri_component(string)
    percent_escape(string, false)
  end

  # -1 where the byte is not a hex digit, which is how a "%" that does
  # not begin an escape is recognised without looking twice.
  def self.hex_value(b)
    return b - 0x30 if b >= 0x30 && b <= 0x39
    return b - 0x41 + 10 if b >= 0x41 && b <= 0x46
    return b - 0x61 + 10 if b >= 0x61 && b <= 0x66
    -1
  end

  def self.percent_unescape(string, plus_as_space)
    out = +""
    i = 0
    n = string.bytesize
    while i < n
      b = string.getbyte(i)
      # A "%" needs two more bytes AND both of them hex. "100%" and
      # "%zz" are not escapes and CRuby leaves them where they are;
      # "%%41" is the same rule applied twice, and answers "%A".
      if b == 0x25 && i + 2 < n
        hi = hex_value(string.getbyte(i + 1))
        lo = hex_value(string.getbyte(i + 2))
        if hi >= 0 && lo >= 0
          out << (hi * 16 + lo).chr
          i += 3
          next
        end
      end
      if b == 0x2B && plus_as_space
        out << " "
      else
        out << b.chr
      end
      i += 1
    end
    out
  end

  def self.unescape(string)
    percent_unescape(string, true)
  end

  def self.unescapeURIComponent(string)
    percent_unescape(string, false)
  end

  def self.unescape_uri_component(string)
    percent_unescape(string, false)
  end

  # The five CRuby writes. `'` is `&#39;` and not `&apos;` -- the named
  # form is not in HTML 4, which is the grammar CRuby escapes to, and
  # the numeric one is read by everything.
  def self.escapeHTML(string)
    out = +""
    i = 0
    n = string.length
    while i < n
      c = string[i]
      if c == "&"
        out << "&amp;"
      elsif c == "\""
        out << "&quot;"
      elsif c == "<"
        out << "&lt;"
      elsif c == ">"
        out << "&gt;"
      elsif c == "'"
        out << "&#39;"
      else
        out << c
      end
      i += 1
    end
    out
  end

  def self.escape_html(string)
    escapeHTML(string)
  end

  def self.h(string)
    escapeHTML(string)
  end

  # A codepoint as UTF-8 bytes. `Integer#chr` takes no encoding here, so
  # the four ranges are written out; they are RFC 3629's.
  def self.utf8_append(out, cp)
    if cp < 0x80
      out << cp.chr
    elsif cp < 0x800
      out << (0xC0 | (cp >> 6)).chr
      out << (0x80 | (cp & 0x3F)).chr
    elsif cp < 0x10000
      out << (0xE0 | (cp >> 12)).chr
      out << (0x80 | ((cp >> 6) & 0x3F)).chr
      out << (0x80 | (cp & 0x3F)).chr
    else
      out << (0xF0 | (cp >> 18)).chr
      out << (0x80 | ((cp >> 12) & 0x3F)).chr
      out << (0x80 | ((cp >> 6) & 0x3F)).chr
      out << (0x80 | (cp & 0x3F)).chr
    end
    out
  end

  # The body of an entity -- what stood between "&" and ";" -- decoded,
  # or nil where it names nothing. Passing nil back rather than "" is
  # what lets `&#0;` decode to a NUL and still be told apart from an
  # entity that did not decode at all.
  def self.decode_entity(body)
    return "&" if body == "amp"
    return "\"" if body == "quot"
    return "<" if body == "lt"
    return ">" if body == "gt"
    return "'" if body == "apos"
    return nil if body.length < 2 || body[0] != "#"
    digits = body[1, body.length - 1]
    hex = digits[0] == "x" || digits[0] == "X"
    digits = digits[1, digits.length - 1] if hex
    return nil if digits.empty?
    cp = 0
    i = 0
    while i < digits.length
      v = hex_value(digits.getbyte(i))
      return nil if v < 0 || (!hex && v > 9)
      cp = cp * (hex ? 16 : 10) + v
      # Past the last codepoint there is nothing to decode to, and
      # CRuby leaves "&#999999999;" standing as it was written.
      return nil if cp > 0x10FFFF
      i += 1
    end
    utf8_append(+"", cp)
  end

  def self.unescapeHTML(string)
    out = +""
    i = 0
    n = string.length
    while i < n
      c = string[i]
      if c != "&"
        out << c
        i += 1
        next
      end
      # The ";" that ends it, if there is one within reach. CRuby's
      # pattern bounds the name, and an "&" with no ";" after it -- the
      # bare "&" of "a & b" -- is just an ampersand.
      j = i + 1
      semi = -1
      while j < n && j - i <= 12
        if string[j] == ";"
          semi = j
          j = n
        else
          j += 1
        end
      end
      decoded = semi < 0 ? nil : decode_entity(string[i + 1, semi - i - 1])
      if decoded.nil?
        out << c
        i += 1
      else
        out << decoded
        i = semi + 1
      end
    end
    out
  end

  def self.unescape_html(string)
    unescapeHTML(string)
  end
end
