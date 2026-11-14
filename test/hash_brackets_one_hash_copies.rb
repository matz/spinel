# Hash[h] makes a new Hash of the pairs (no default), not h itself, for a
# typed Hash and for a value known only at run time.
x = { a: 1 }
y = Hash[x]
x.clear
p y
p y.equal?(x)
s = { "k" => "v", "j" => "w" }
t = Hash[s]
t["z"] = "q"
p s, t
d = Hash.new(5)
d[1] = 2
e = Hash[d]
p e[9], e
f = { 1 => 2.5 }
g = Hash[f]
p g.equal?(f), g
p Hash[[[1, 2]]]
def pick(n) = n > 0 ? { a: 1 } : [[:b, 2]]
x2 = pick(1)
y2 = Hash[x2]
x2.clear
p y2, y2.equal?(x2)
def m(h) = Hash[h]
h2 = { 1 => 2 }
k = m(h2)
h2[3] = 4
p k, h2
z = pick(0)
p Hash[z]
w = [[:q]]
p Hash[w]
