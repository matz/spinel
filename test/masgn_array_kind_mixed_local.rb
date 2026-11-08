# A multiple assignment that binds a local to an Array of another kind than
# the local's other writes: the Array is the general Array, so a later push
# of another kind of element works, as with a plain write.
f = [1, "q"]
g, f = 1, [3, 4]
f << "z"
p f, g

h = [1, "q"]
h, k = [5, 6], 2
h.push(:s)
p h, k

m = [3, 4]
n, m = 0, [1, "q"]
m << 2.5
p m, n

def pair = [7, 8]
r = [1, "q"]
s, r = 1, pair
r << "z"
p r, s

a = [3, 4]
t = [1, "q"]
u, t = 1, a
t << "z"
p t, a, u

w = [1.5]
x, w = 1, [3, 4]
w << 2.5
p w, x

2.times do |i|
  v = [1, "q"]
  y, v = i, [3, 4]
  v << "z"
  p v, y
end

# both writes are multiple assignments, the typed one first
o, q = 1, [3, 4]
q << "z"
p q
o, q = 2, [1.5]
p q, o

# an Array that only passes through the target keeps its own kind
big = Array.new(5) { |i| i }
z = [1, "q"]
zz, z = 1, big
p z.size, big.sum, zz
