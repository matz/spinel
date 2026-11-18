def capture(value, pattern, group)
  value[pattern, group]
end

value = ["a\0b".b, 11][0]
pattern = [/(?<middle>\x00)./, 11][0]
whole = capture(value, pattern, 0)
numbered = capture(value, pattern, 1)
named = capture(value, pattern, :middle)
none = capture(value, pattern, 2)
held = [whole, numbered, named, 11]
p [held[0].bytes, held[1].bytes, held[2].bytes, none]
p [held[0].encoding.name, held[1].encoding.name, held[2].encoding.name]
whole << "x" * 180
numbered << "x" * 180
named << "x" * 180
p [held[0].bytesize, held[1].bytesize, held[2].bytesize]
