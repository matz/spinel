# String#byteindex and #byterindex raise IndexError for an offset inside a
# character, as CRuby's str_ensure_byte_pos does: either end of the String
# is a boundary, byterindex clamps a large offset to the end first, and a
# BINARY String has a boundary at every byte.
def t
  yield.inspect
rescue => e
  "#{e.class}: #{e.message}"
end
s = "aéb€c"
(0..s.bytesize + 1).each { |i| puts "idx #{i}: #{t { s.byteindex("c", i) }} | #{t { s.byterindex("a", i) }} | #{t { s.byteindex(/c/, i) }} | #{t { s.byterindex(/a/, i) }}" }
puts t { s.byteindex("c", -1) }
puts t { s.byteindex("c", -2) }
puts t { s.byterindex("a", -8) }
puts t { "a=1&\x81b=2".byteindex("b", 4) }
puts t { "a=1&\x81b=2".byteindex("b", 5) }
puts t { "\x81b".byteindex("b", 0) }
puts t { "é".b.byteindex("\xA9".b, 1) }
puts t { "aé".byteindex("", 2) }
