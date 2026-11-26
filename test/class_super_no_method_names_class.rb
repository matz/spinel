# A class method's super that reaches no method raises NoMethodError naming the
# receiving class as CRuby does: by what its class method `name` answers when
# that is a String, else by its path, or, for an anonymous class, its inspect.
def msg = yield rescue "#{$!.class}: #{$!.message.sub(/0x\h+/, "0x")}"

class A
  def self.name = "AN"
  def self.nope = super
end
class B < A; end
x = Class.new(A)
p msg { A.nope }, msg { B.nope }, msg { x.nope }
p [msg { A.nope }, msg { x.nope }]
def tail(k) = k.nope
p msg { tail(x) }, msg { tail(B) }

# nil passes on to the path or the inspect; any other value is the address form
class D
  def self.name = nil
  def self.nope = super
end
class D2 < D
  def self.inspect = "D2I"
end
d = Class.new(D)
e = Class.new(D2)
p msg { D.nope }, msg { d.nope }, msg { D2.nope }, msg { e.nope }
class C
  def self.name = 5
  def self.inspect = "CI"
  def self.nope = super
end
p msg { C.nope }

# from a module the class extends, a `class << self`, a parent, a module itself
module NM
  def name = "FromExt"
end
class E
  extend NM
  def self.nope = super
end
class F
  class << self
    def name = "FS"
    def nope = super
  end
end
class P
  def self.nope = super
end
class Q < P
  def self.name = "QN"
end
q = Class.new(Q)
r = Class.new(P)
module M
  def self.name = "MN"
  def self.nope = super
end
p msg { E.nope }, msg { F.nope }, msg { P.nope }, msg { Q.nope }, msg { q.nope }, msg { r.nope }, msg { M.nope }

# a name that raises, or takes an argument it is not given: the path or inspect
class G
  def self.name = raise("boom")
  def self.nope = super
end
g = Class.new(G)
class I
  def self.name(x = 1) = "I#{x}"
  def self.nope = super
end
class I2
  def self.name(x) = "I2"
  def self.nope = super
end
p msg { G.nope }, msg { g.nope }, msg { I.nope }, msg { I2.nope }

# a core method's name
class U
  def self.name = "UN"
  def self.upcase = super
end
u = Class.new(U)
p msg { U.upcase }, msg { u.upcase }

# a nil-safe call and a boxed receiver
class K
  def self.name = "KN"
  def self.nope = super
  def self.cn(v) = v&.nope
end
a = Class.new(K)
p K.cn(nil), msg { K.cn(a) }
h = {a: a, b: 1}
w = h[:a]
p msg { w.nope }

# the message is built when it is read, from the class as it is then: a
# rescue that does not read it calls no name, two reads call it twice, and a
# class named after the raise answers its new name
$names = 0
class L
  def self.name = ($names += 1; "L")
  def self.nope = super
end
begin; L.nope; rescue NoMethodError; end
p $names
e = begin; L.nope; rescue NoMethodError => x; p x.class; x; end
p $names
e.message; e.message
p $names
p e.name, e.receiver, e.args, e == e.dup, $names
p e.exception("other").message, $names
k = Class.new { def self.nope = super }
e = begin; k.nope; rescue NoMethodError => x; x; end
LNamed = k
p e.message
