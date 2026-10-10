# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# An Integer Hash whose default is -2**63, and fetch with -2**63 as the
# default: the default is a number, not a miss.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
h = Hash.new(m)
h[1] = 5
p h[1]
p h[2]
p h[2].nil?
p(h[2] ? 1 : 0)
p h.default
puts "#{h[2]}"
p h.fetch(1, m)
p h.fetch(2, m)
p h.fetch(2, m).nil?
p h.fetch(2) { m }
g = { 1 => 0 }
p g.fetch(1, m)
p g.fetch(9, m)
p g.fetch(9, m).nil?
p g.fetch(9, m) + 1
p g.fetch(9, nil)
p g.fetch(9, nil).nil?
g[9] = m
p g.fetch(9, 0)
p g.fetch(9, nil)
p g.fetch(9) { 0 }
g.default = m
p g[7]
p g[7].nil?
p g.default
