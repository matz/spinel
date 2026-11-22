# Minimal TOML reader for spin (M0). Covers the spin.toml subset: [tables],
# key = "string", key = { k = "v", ... } inline tables, and comments. This
# ships inside spin for bootstrap and graduates to the stdlib `toml` feature
# later (docs/spin.md §7). Storage is deliberately string->string only.

class TomlDoc
  def initialize
    @vals = { "" => "" }    # "table\x01key" => plain string value
    @inline = { "" => "" }  # "table\x01key\x01ik" => inline-table member
    @keys = { "" => "" }    # "table" => newline-joined key list
    @counts = { "" => 0 }   # "[[base]]" occurrence count; entries live at "base.<i>"
    @arrays = { "" => "" }  # "table\x01key" => "1" for a value written as an array
  end

  def self.parse(text)
    doc = TomlDoc.new
    doc.parse_text(text)
    doc
  end

  def parse_text(text)
    table = ""
    text.split("\n").each do |line0|
      line = strip_comment(line0).strip
      next if line == ""
      if line.start_with?("[[") && line.end_with?("]]")
        base = line[2..-3].strip
        n = @counts.key?(base) ? @counts[base] : 0
        @counts[base] = n + 1
        table = base + "." + n.to_s
        next
      end
      if line.start_with?("[") && line.end_with?("]")
        table = line[1..-2].strip
        next
      end
      eq = line.index("=")
      next if eq.nil?
      key = line[0..(eq - 1)].strip
      val = line[(eq + 1)..-1].strip
      remember(table, key, val)
    end
  end

  def strip_comment(line)
    out = ""
    in_str = false
    esc = false
    line.each_char do |ch|
      if esc
        esc = false
      elsif ch == "\\" && in_str
        esc = true
      elsif ch == "\""
        in_str = !in_str
      end
      break if ch == "#" && !in_str
      out += ch
    end
    out
  end

  def remember(table, key, val)
    have = @keys.key?(table) ? @keys[table] : ""
    @keys[table] = have == "" ? key : (have + "\n" + key)
    if val.start_with?("{") && val.end_with?("}")
      inner = val[1..-2]
      split_top(inner).split("\x02").each do |pair|
        peq = pair.index("=")
        next if peq.nil?
        ik = pair[0..(peq - 1)].strip
        iv = pair[(peq + 1)..-1].strip
        # an array member keeps its bracket text (inline_array parses it);
        # a plain member unquotes as before
        iv = unquote(iv) unless iv.start_with?("[")
        @inline[table + "\x01" + key + "\x01" + ik] = iv
      end
      @vals[table + "\x01" + key] = ""
    else
      # (a quoted "[...]" is a string: only bracket text is an array)
      @arrays[table + "\x01" + key] = "1" if val.start_with?("[")
      @vals[table + "\x01" + key] = unquote(val)
    end
  end

  # Split inline-table members on top-level commas only, so an array value
  # (`features = ["a", "b"]`) survives as one member. Returns the pieces
  # \x02-packed (a String[] would go poly downstream).
  def split_top(inner)
    out = ""
    depth = 0
    in_str = false
    piece = ""
    esc = false
    inner.each_char do |ch|
      # (a \" inside a string is part of it, not its end)
      if esc
        esc = false
      elsif ch == "\\" && in_str
        esc = true
      elsif ch == "\""
        in_str = !in_str
      end
      depth += 1 if ch == "[" && !in_str
      depth -= 1 if ch == "]" && !in_str
      if ch == "," && depth == 0 && !in_str
        out += "\x02" unless out == "" && piece == ""
        out += piece
        piece = ""
      else
        piece += ch
      end
    end
    out += "\x02" unless out == "" || piece == ""
    out + piece
  end

  # a basic string's text, with \" and \\ read as " and \
  def unquote(s)
    return s unless s.length >= 2 && s.start_with?("\"") && s.end_with?("\"")
    out = ""
    esc = false
    s[1..-2].each_char do |ch|
      if esc
        out += "\\" unless ch == "\"" || ch == "\\"
        out += ch
        esc = false
      elsif ch == "\\"
        esc = true
      else
        out += ch
      end
    end
    out += "\\" if esc
    out
  end

  # whether `key` is set in `table`: true for a string, an array or an inline
  # table
  def key?(table, key)
    @vals.key?(table + "\x01" + key)
  end

  # plain value, "" when absent (or when the value is an inline table)
  def get(table, key)
    k = table + "\x01" + key
    return "" unless @vals.key?(k)
    @vals[k]
  end

  # `key = ["a", "b"]` string-array value -> newline-packed string ("" when
  # absent or not an array). Raw array text passes through remember/unquote
  # untouched (it neither starts with `{` nor is a bare quoted string), so
  # the bracket text is intact here. Newline packing over a String[] return
  # keeps the value monomorphic for spinel (house style, cf. dep_srcs).
  def get_array(table, key)
    return "" unless array?(table, key)
    array_text(get(table, key))
  end

  # whether `key` in `table` was written as an array (not a quoted string)
  def array?(table, key)
    @arrays.key?(table + "\x01" + key)
  end

  # inline-table member holding an array (`dep = { features = ["a"] }`),
  # newline-packed; "" when absent or not an array
  def inline_array(table, key, ik)
    array_text(get_inline(table, key, ik))
  end

  def array_text(raw)
    return "" unless raw.start_with?("[") && raw.end_with?("]")
    out = ""
    # ["a,b"] is one element: split_top splits on a comma outside quotes
    split_top(raw[1..-2]).split("\x02").each do |part|
      v = unquote(part.strip)
      next if v == ""
      out += "\n" unless out == ""
      out += v
    end
    out
  end

  # inline-table member, "" when absent
  def get_inline(table, key, ik)
    k = table + "\x01" + key + "\x01" + ik
    return "" unless @inline.key?(k)
    @inline[k]
  end

  # number of [[base]] entries; each is a table named "base.<i>"
  def array_len(base)
    @counts.key?(base) ? @counts[base] : 0
  end

  def table_keys(table)
    return [] unless @keys.key?(table)
    joined = @keys[table]
    return [] if joined == ""
    joined.split("\n")
  end

  def tables(prefix)
    names = []
    @keys.each_key do |table|
      names.push(table) if table.start_with?(prefix + ".")
    end
    names.sort
  end
end
