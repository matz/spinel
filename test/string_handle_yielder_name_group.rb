# A method that appends to a String parameter shares its name with a
# yielding method that names its block: the String is the caller's, as in
# CRuby. A parameter is lent the caller's slot only when every method of
# the name can take one, and a yielder that names its block was counted in
# while it is spliced like any yielder, or was refused while the lowering
# turns it into a proc call; either way the other methods kept a copy.

X = "x"

def m(p1, &b) = yield(p1)
class N; def m(p1) = (p1 << X; nil); end
m(+"v") { |x| x }
w = +"s1"; N.new.m(w); p w

def k(p1, &b) = (@kept = b; yield(p1))
class K; def k(p1) = (p1 << X; nil); end
k(+"v") { |x| x }
w = +"s2"; K.new.k(w); p w

def r(p1, n = 0, &b) = n > 2 ? yield(p1) : r(p1, n + 1, &b)
class R; def r(p1) = (p1 << X; nil); end
r(+"v") { |x| x }
w = +"s3"; R.new.r(w); p w

def a(p1, &b) = (p1 << "y"; yield(p1))
class A; def a(p1) = (p1 << X; nil); end
v = +"v"; a(v) { |x| x }; p v
w = +"s4"; A.new.a(w); p w

class B; def g(p1, &b) = yield(p1); end
class G; def g(p1) = (p1 << X; nil); end
B.new.g(+"v") { |x| x }
w = +"s5"; G.new.g(w); p w
z = +"s6"; [B.new, G.new][1].g(z); p z
