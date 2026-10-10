# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A nil that reaches an Integer-keyed Hash through a nullable Integer
# variable (a missed index) is a nil key, as in CRuby: stored, read, listed,
# deleted. A present -2**63 through the same variable is a number key.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
a = [1, 2]
k = a.index(9)
h = { 1 => 2 }
p h[k]
p h.key?(k)
h[k] = 3
p h
p h[k]
p h[nil]
p h.key?(k)
p h.keys
p h.size
p h.fetch(k, 0)
p h.fetch(k) { 7 }
p h.dig(k)
p h.delete(k)
p h
p h.delete(k)
s = { 1 => "a" }
p s[k]
p s.key?(k)
s[k] = "z"
p s
p s[k]
j = a.index(2)
p h[j]
h[j] = 4
p h
t = [m, 3].index(m)
g = { 0 => 0 }
g[[m, 3][t]] = 9
p g
p g[m]
p g.keys
u = [m].find { |e| e < 0 }
p g[u]
p g.key?(u)
w = [m].find { |e| e > 0 }
p g[w]
p g.key?(w)
g[w] = 1
p g.keys
