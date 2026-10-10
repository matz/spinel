# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Iterating a Range whose begin is -2**63: for, each, step, upto / downto,
# reverse_each, first / last, min / max, minmax. The begin is a bound, not
# the beginless marker.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
for i in m..m + 2
  p i
end
(m..m + 1).each { |i| p i - m }
(m...m + 2).each_with_index { |i, k| p [k, i == m] }
p (m..m + 3).step(2).to_a
p m.step(m + 4, 2).to_a
m.upto(m + 1) { |i| p i }
(m + 1).downto(m) { |i| p i }
p (m..m + 2).reverse_each.to_a
p (m..m + 2).first
p (m..m + 2).first(2)
p (m..m + 2).last
p (m..m + 2).last(2)
p (m..m + 2).min
p (m..m + 2).max
p (m..m + 2).minmax
p (m...m).min
p (m...m).max
p (m..m + 2).count
p (m..m + 2).to_a.size
p (m..m + 2).each_slice(2).to_a
p (m..m + 2).each_cons(2).to_a
p (m..m + 2).select(&:even?)
p (m..m + 2).map { |i| i & 1 }
p (m..m + 2).include?(m + 1)
p (m..m + 2) === m
p (m..).first(1)
p (m..).each.first
p (..m + 1).include?(m)
e = (m..m + 1).each
p e.next, e.next
p (m..m + 2).lazy.map { |i| i - m }.first(2)
