# byteoffset, match and match_length raise IndexError for a group index
# that is negative or past the last group, as begin/end/offset do.
m = /(a)(b)?(c)/.match("ac")

def try
  r = yield
  p r
rescue IndexError => e
  puts "IndexError: #{e.message}"
end

[-1, -4, 4, 9].each do |i|
  try { m.byteoffset(i) }
  try { m.match(i) }
  try { m.match_length(i) }
end

# groups in range, matched and unmatched
[0, 1, 2, 3].each do |i|
  try { m.byteoffset(i) }
  try { m.match(i) }
  try { m.match_length(i) }
end

# a literal index
try { m.byteoffset(-1) }
try { m.match(-2) }
try { m.match_length(5) }

# names still work
n = /(?<x>a)(?<y>b)?/.match("a")
try { n.match(:x) }
try { n.match("y") }
try { n.match_length(:x) }
try { n.byteoffset(:y) }
