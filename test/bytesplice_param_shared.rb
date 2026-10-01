# bytesplice and append_as_bytes mutate their receiver in place, as the
# other String mutators do: a parameter, a block's or a proc's parameter
# they are called on is the caller's own String, as in CRuby. They were not
# counted among the mutators, so the method got a copy and the caller's
# String never changed.

def f(a) = a.bytesplice(0, 1, "Q")
s = +"abc"; p f(s), s
def g(a) = a.append_as_bytes("Z")
t = +"abc"; p g(t), t
def h(a) = (a.bytesplice(0..0, "R"); 0)
u = +"abc"; h(u); p u
h(1) rescue p $!.class

class K
  def initialize = (@s = +"abc")
  def go = (f(@s); @s)
end
p K.new.go
[+"abc", +"def"].each { |w| h(w); p w }
hs = { k: +"ghi" }; h(hs[:k]); p hs

pr = proc { |x| x.bytesplice(0, 1, "P") }
v = +"abc"; pr.call(v); p v
def y(v) = yield(v)
w = +"abc"; y(w) { |q| q.append_as_bytes("Y") }; p w
m = method(:h); z = +"abc"; m.call(z); p z
l = ->(x) { x.append_as_bytes("!") }; o = +"o"; l.(o); p o

def k2(a, b) = (b.append_as_bytes(a); 0)
fz = "fr".freeze
p(begin; k2("Z", fz); rescue FrozenError => e; e.class; end)

# every String method that changes its receiver, on a parameter
def m1(a) = (a.setbyte(0, 81); 0)
def m2(a) = (a.force_encoding("ASCII-8BIT"); 0)
def m3(a) = (a.freeze; 0)
def m4(a) = (a.slice!(0); 0)
def m5(a) = (a.encode!("ASCII-8BIT"); 0)
def m6(a) = (a.delete_suffix!("c"); 0)
s = +"abc"; m1(s); p s
s = +"abc"; m2(s); p s.encoding == Encoding::BINARY
s = +"abc"; m3(s); p s.frozen?
s = +"abc"; m4(s); p s
s = +"abc"; m5(s); p s.encoding == Encoding::BINARY
s = +"abc"; m6(s); p s
