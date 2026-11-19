# spinel: gc-minor
s = +"abc a"
result = s.scan(/(a)(bc)?()/)
result[0][0] << "!"
result[1][2] << "?"
p result
seen = []
s.scan(/(a)(bc)?()/) do |row|
  row[0] << "."
  seen << row
end
p seen
p s

# Counted binary spans retain both the NUL and their encoding.
binary = "\xff\x00".b.scan(/(\xff\x00)(z)?/n)
binary[0][0] << "?"
p [binary[0][0].bytes, binary[0][0].encoding.name, binary[0][1]]

# A runtime pattern can yield rows or whole matches.
def scan_with(pattern) = "aa".scan(pattern)
groups = scan_with(/(a)(z)?/)
groups[0][0] << "!"
p groups
flat = scan_with(/a/)
flat[0] << "?"
p flat
