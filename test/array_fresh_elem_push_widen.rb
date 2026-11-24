# A push of another kind into a new Array a builtin returns, held in a
# Hash value or an Array element, widens the Array, as it does held in a
# local.

h = { a: "ab".bytes }
h[:a] << "q"
p h

a = ["ab".bytes]
a[0] << "q"
p a

b = ["ab".bytes, "cd".bytes]
b[1].push("z", 3)
p b

s = { k: "a b".split }
s[:k] << 1
p s

f = [[1.5, 2.5].map { |x| x * 2 }]
f.first << :x
p f

e = { a: "xy".bytes }
e.each_value { |v| v << "z" }
p e

g = { a: "ab".bytes }
p g[:a].sum

# a read of the Array that adds no row (`dup`, `map`) leaves the widening
c = ["ab".bytes]
d = c.dup
c[0] << "q"
p c, d

m = ["ab".bytes, "cd".bytes]
m[0] << "q"
p m[0][2], m[1][0] + 1, m.map(&:size)

# a nil from a safe-navigation call stays nil
def pick(f) = f ? "ab" : nil
s1 = pick(true)
o1 = { a: s1&.bytes }
o1[:a] << "x" if o1[:a]
r1 = [s1&.bytes]
r1[0] << "x" if r1[0]
p o1, r1

s2 = pick(false)
o2 = { a: s2&.bytes }
o2[:a] << "x" if o2[:a]
r2 = [s2&.bytes]
r2[0] << "x" if r2[0]
p o2, r2
