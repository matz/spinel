class OtherCharacterSource
  def chars
    ["fallback"]
  end
end

def boxed_chars(value)
  value.chars
end

def show_chars(list)
  p list.map { |char| [char.bytes, char.encoding.name] }
end

broken = [0xff, 0xe2, 0x82].pack("C*").force_encoding("UTF-8")
mixed = "é\x00" + broken
show_chars(mixed.chars)
show_chars(boxed_chars(mixed))
show_chars(boxed_chars(OtherCharacterSource.new))
show_chars(mixed.each_char.to_a)
show_chars(mixed.b.chars)
p [broken[1, 2].chr.bytes, mixed.reverse.bytes, mixed.b.reverse.bytes]
p ["\x00a".chr.bytes, "é".b.chr.bytes, "".b.chr.encoding.name]
first = "a".chr
second = "a".chr
first << "b"
p [first, second, first.equal?(second)]
empty = ""
binary_empty = "".b
p [empty.chr.equal?(empty.chr), binary_empty.chr.equal?(binary_empty.chr)]
