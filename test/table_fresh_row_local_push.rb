# A row a builtin returns (`"ab".bytes`, a `map`), held in an Array
# literal and read into a local, takes a push of another kind in place,
# and so does a row a store adds to the literal (`n << s.bytes`).

n = ["ab".bytes]
x = n[0]
x << "q"
p n
p x

a = ["ab".bytes, "cd".bytes]
y = a.first
y.push("q", 2.5)
z = a.fetch(1)
z.unshift(:s)
p a

f = [[1.5, 2.5].map { |v| v * 2 }]
r = f.last
r[0] = "s"
p f

m = [[1, 2].map { |v| v + 1 }]
w = m[0]
w << 1.5
p m

def add(t)
  row = t[0]
  row << "q"
end
t = ["xy".bytes]
add(t)
p t

k = ["ab".bytes]
i = k[0]
i << 5
p k, i.sum

b = ["ab".bytes]
b << "cd".bytes
b[2] = "ef".bytes
b.insert(0, [1, 2])
o = "gh".bytes
b.push(o)
x = b[2]
x << "q"
b.last << "r"
p b, o

def put(t, r) = t << r
c = ["ab".bytes]
put(c, "cd".bytes)
y = c[1]
y << "q"
p c

def mk(s) = s.bytes
o = "ab".bytes
d = [o, ARGV.empty? ? "cd".bytes : "ef".bytes, mk("gh")]
d.concat(["ij".bytes])
rows = ["kl".bytes]
d.push(*rows)
d[0, 0] = ["mn".bytes]
x = d[1]
x << "q"
y = d[3]
y << "r"
d.last << "s"
p d, o

e = ["ab".bytes]
e.replace(["cd".bytes])
z = e[0]
z << "q"
p e

g = []
g << "ab".bytes
x = g[0]
x << "q"
p g

h = [[1, "a"]]
h << "cd".bytes
y = h[1]
y << "q"
p h

j = ["ab".bytes]
j << (ARGV.empty? ? "cd".bytes : "ef".bytes)
z = j[1]
z << "q"
p j

u = ["ab".bytes, "ef".bytes]
u.fill("cd".bytes)
x = u[0]
x << "q"
p u

v = ["ab".bytes]
v[0..0] = ["cd".bytes, "ef".bytes]
y = v[1]
y << "q"
z = v[0]
z << 5
p v, z.sum

lists = [["cd".bytes], ["ef".bytes]]
w = ["ab".bytes]
w.concat(*lists)
x = w[2]
x << "q"
p w

s = ["ab".bytes]
s.replace(s)
s.concat(s)
t2 = ["cd".bytes]
t2.concat(s)
s.concat(t2)
x = s[0]
x << "q"
y = t2[0]
y << :s
p s, t2
