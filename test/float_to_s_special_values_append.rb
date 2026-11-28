# Appending to the String Float#to_s answers for 0.0, -0.0, Infinity and NaN:
# the String has its length, so the append joins (it read as empty: the
# fixed texts were returned with the length unset).
[0.0, -0.0, 1.0, 0.1, 1e20, Float::INFINITY, Float::NAN].each do |f|
  s = f.to_s
  s << "a"
  p s
end
s = 0.0.inspect
s << "abc"
s.concat("d")
p s, s.size
