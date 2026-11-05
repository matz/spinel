# A `next` with a String in a Hash#map block whose tail is a String too: the
# block's value slot is the bytes face, and the `next` stored its value as
# the handle, so the C did not compile ("incompatible pointer types
# assigning to 'const char *' from 'sp_String *'"). The `next` of a block
# parameter, of a local and of a pack all did it; an Array of pairs, a tail
# that is the parameter itself and a `next` of a literal did not.
# CRuby: ["a", "bx"], ["a", "bx"], ["bool", "x!"], ["a", "bx"],
# [1, 97, 1, 1, 98, 2, 120, 1, 99, 1], {"a" => true}.
pairs = { "a" => true, "b" => "x" }
p(pairs.map { |key, value| next key if value == true; key + value })
p(pairs.map { |key, value| bool = !!value == value; next key if bool; key + value })
p(pairs.map { |key, value| next "bool" if value == true; value + "!" })
p(pairs.map { |key, value| if value == true then key else key + value end })
headers = { "a" => true, "b" => "x", "c" => false }
encoded = headers.map do |key, value|
  encoded_key = [key.bytesize, key].pack('Ca*')
  next [encoded_key, [1].pack('C')].pack('a*a*') if !!value == value
  [encoded_key, [2].pack('C'), value].pack('a*a*a*')
end
p encoded.join.bytes
p(pairs.select { |key, value| next true if value == true; value.size == 2 })
