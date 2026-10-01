# A local bound to a container held by another container, and a String
# mutated through an element of it: CRuby mutates the String the outer
# container holds. The local was a POLY name for the inner container, its
# stores were not walked, and the element was a copy.

a = [[+"x"]]
x = a[0]
x[0] << "!"
p a
b = [[+"x", 1], [+"y"]]
y = b[1]
y[0].upcase!
y2 = b.first
y2[0] << "?"
p b
h = {k: [+"h"], n: 1}
z = h[:k]
z[0] << "!"
p h
hh = {k: {j: +"v"}}
w = hh[:k]
w[:j] << "#"
p hh
s = [[+"s1"], [+"s2"]]
t = s[0]
u = t
u[0] << "+"
p s
def run
  m = [[+"m"], 2]
  e = m[0]
  e[0].concat("!")
  p m
end
run
def via(arr)
  i = arr[0]
  i[0] << "~"
  arr
end
p via([[+"p"]])
q = [[+"q"]]
r = q.last
r.first << "*"
p q
