# `x ||= v` on an Integer or Float local that some other write also
# assigns. The slot was declared with the type's zero and its `||=`
# compiled to nothing unless every write was a `||=`, so the or-write was
# lost whenever it ran before the definite write: an op-assign after it, a
# definite write in a branch not taken, or a block local, which starts each
# iteration nil.
def f
  v ||= 5
  v += 2
  p v
end
f

def g(k)
  w ||= 1.5
  w *= k
  p w
end
g(2)

[2].each { |x| s ||= 0; s += x; p s }
[2].each { |x| u ||= 0.5; u += x; p u }
[3].each { |x| r ||= 1; r = r * x; p r }

def h(c)
  x = 1 if c
  p x
  x ||= 5
  p x
end
h(false)
h(true)

# a definite write ahead of it keeps its value
def n
  x = 0
  x ||= 5
  y = (z ||= 3)
  z += 1
  p [x, y, z]
end
n

i = 0
while i < 3
  t ||= 10
  t += i
  i += 1
end
p t

def fl(c)
  q = 2.5 if c
  r = (q ||= 1.5) * 2
  p [q, r]
end
fl(false)
fl(true)
