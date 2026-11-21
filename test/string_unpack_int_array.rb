# String#unpack with a literal format of integer directives returns an
# Integer Array, as the values are read without boxing, and a slot the
# String is too short for is nil as in CRuby.
def sum(data)
  words = data.unpack("V*")
  t = 0
  words.each { |w| t += w }
  t
end
p sum("\x01\x00\x00\x00\x02\x00\x00\x00".b)

data = "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x80\x7f\xff".b
p data.unpack("C*")
p data.unpack("c*")
p data.unpack("n*")
p data.unpack("v3")
p data.unpack("N2")
p data.unpack("V*")
p data.unpack("s<2S>2")
p data.unpack("l<L>")
p data.unpack("i!I_")
p data.unpack("nCv")
p data.unpack("N4")
p "a".unpack("CC")
p "".unpack("C*")
p "hé€".unpack("U*")
p "hé€".unpack("U2C")

begin
  "\xff".b.unpack("U*")
rescue ArgumentError => e
  p [e.class, e.message]
end

words = data.unpack("n2")
words << "str"
words << nil
p words
padded = "\x01".b.unpack("C3")
p padded.compact, padded.include?(nil)
a, b, c = "\x00\x01\x00\x02".b.unpack("nnn")
p [a, b, c]
p((["%.4x"] * 4).join(":") % "\x20\x01\x0d\xb8\x00\x00\x00\x01".b.unpack("n4"))

def push_str(arr) = arr << "q"
pushed = "ab".unpack("C*")
push_str(pushed)
p pushed

short = "\x01".b.unpack("C3")
short.each { |v| p v }
p short.map { |v| v.nil? }
p "\x01\x02\x03".b.unpack("CC4").map { |v| v.nil? ? -1 : v * 2 }
