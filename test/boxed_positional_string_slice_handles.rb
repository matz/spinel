def slice(value, start, length)
  value[start, length]
end

string = ["ab\0c".b, 83][0]
symbol = [:abcd, 83][0]
from_string = slice(string, 1, 2)
from_symbol = slice(symbol, 1, 1)
empty = slice(string, 4, 1)
miss = slice(string, 5, 1)
p [from_string.bytes, from_string.encoding.name, from_string.frozen?]
p [from_symbol.bytes, from_symbol.frozen?, empty.encoding.name, miss]
held_string = [from_string, 0]
held_symbol = [from_symbol, 0]
held_empty = [empty, 0]
from_string << "x" * 180
from_symbol << "x" * 180
empty << "x" * 180
p [held_string[0].bytesize, held_symbol[0].bytesize, held_empty[0].bytesize]
