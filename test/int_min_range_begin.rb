# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A Range whose begin is -2**63 is a Range with that begin, not a beginless
# one; a beginless Range is still beginless.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
r = (m..m + 2)
p r
p r.begin
p r.begin.nil?
p r.end
p r.size
p r.to_a
p r.include?(m)
p r.cover?(m + 1)
p r.min, r.max
p r.first(2)
p r.each.to_a
p (m...m).to_a
p (m...m + 1).size
p (m..).begin
p (m..).first(2)
p (m..).include?(m)
p (..m).end
p (..m).include?(m)
p (..m).begin.nil?
p (m..m).to_a
p r.map { |e| e - m }
p r.step(2).to_a
case m
when (m..0) then puts "in range"
else puts "out"
end
s = "abcdef"
p s[(m..)]
p [1, 2, 3][(m..)]
a = [5, 6, 7]
i = a.index(9)
p a[(i || 0)..]
