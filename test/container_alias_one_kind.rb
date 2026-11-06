# Two names of one Hash or Array that are given values of different
# classes hold the same container: a store through either is seen through
# the other, and equal? holds (#10930). Each name used to take its own
# container kind, and an assignment between them converted by copying.
# The names may meet directly, through a parameter, or through returns
# any number of methods deep; a dup is a container of its own.

def inner(h) = h
def middle(h) = inner(h)
def outer(h) = middle(h)

h = {}
other = outer(h)
h["s"] = "value"
other["i"] = 7
p h.equal?(other), h.size, other.size, h["i"], other["s"]

g = {}
g2 = g
g["s"] = "value"
g2["i"] = 7
p g.equal?(g2), g, g2

def id(x) = x
a = []
b = id(a)
a << "s"
b << :sym
p a.equal?(b), a, b

def d4(x) = d3(x)
def d3(x) = d2(x)
def d2(x) = d1(x)
def d1(x) = x
k = []
k2 = d4(k)
k << 1.5
k2 << "two"
p k.equal?(k2), k.size, k2

orig = {"a" => "1"}
copy = orig.dup
copy["x"] = 1
p orig, copy, orig.equal?(copy)
