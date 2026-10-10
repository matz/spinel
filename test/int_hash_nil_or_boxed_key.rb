# An Integer-keyed Hash looked up with a key that can be nil, or with a boxed
# key that may hold another class: nil and the other classes match no entry
# (CRuby looks them up and misses), an Integer key finds its entry.

n = ARGV.size
h = { 1 => 10, 2 => 20 }
k = [1, 2].index(9)          # nil
j = [1, 2].index(2)          # 1
b = [1, "x", 2.0, nil][n]    # a boxed key: 1
x = [1, "x", 2.0, nil][n + 1]
p h[k], h[j], h[b], h[x]
p h.fetch(k, 0), h.fetch(j, 0), h.fetch(x, 0)
p h.fetch(k) { -1 }, h.fetch(j) { -1 }, h.fetch(x) { -2 }
p h.fetch(k) { |q| q.nil? ? :nil_key : q }
p h.fetch(j) { |q| q }
p h.slice(k, j, x, b)
p h.except(k, x)
p h.assoc(k), h.assoc(j), h.assoc(x)
d = { 1 => 10, 2 => 20 }
p d.delete(k), d.delete(x), d
p d.delete(j), d
