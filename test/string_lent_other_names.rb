# A String lent to a parameter that appends to it is the one String every
# other name for it holds, as in CRuby: a local or ivar alias, an Array's
# element, an ivar it was stored in, a parameter it came from. The callee
# wrote its grown String back through the lent slot only, and every other
# name kept the old one. A parameter rebound after its append took a copy,
# and a String both lent to a slot and handed to a handle or boxed
# parameter appended through one of them to a copy.

X = "!" * 100

def gr(x) = x << X
class C1
  def go = (@x = +"k"; y = @x; gr(@x); y.size)
end
p C1.new.go

def g5(a) = (a << "!"; nil)
def outer(y) = (o = y; g5(y); o)
p outer(+"D")

def grow(v) = v << "!"
v = +"a"; a = [v]; grow(v); p a[0]
def one(s) = s << X
s = +"x"; arr = [s]; one(s); p arr[0].size

module Helper; def self.hfill_into(d) = (d << "x"; d); end
class V1; def go = (@buf = +"a"; keep = @buf; Helper.hfill_into(@buf); keep); end
p V1.new.go
def fill_into(io) = (io << "x" * 40; nil)
class V2; def go = (io = String.new; @kept = io; fill_into(io); [io.size, @kept.size]); end
p V2.new.go

def grow3(b, x, n) = b << x
h = {k: +"a"}
grow3(h[:k], "y", 1)
class C3; def go = (@buf = +"b"; grow3(@buf, "x", 9); @buf); end
p C3.new.go, h

def m(io) = (io << "r1"; io = String.new; io << "r2"; nil)
z = +""; m(z); p z

def grow4(k) = k << "x"
def app4(k) = k << "y"
mm = method(:grow4)
k = +"k"; grow4(k); app4(k); p k, mm.arity

def lent(q) = q << "1"
def poly(q) = q << "2"
poly(5) rescue nil
w = +"s"; lent(w); poly(w); p w

def grow5(a, b, *r, z) = (a << X; r.size)
class Gath; def initialize = (@buf = +"I"); def run(xs) = (grow5(@buf, *xs, 9); @buf.size); end
p Gath.new.run([1])
u = +"U"; o = u; o << "?"; grow5(u, *[1], 2); p o.size
