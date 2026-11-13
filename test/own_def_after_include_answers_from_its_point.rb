# A class's own def of a name a module it includes also defines answers
# only from the point the def runs: before it, the module's method
# answers, at the top level, in a method body and in a block, on a
# receiver that may be either of two classes, through a send and through
# a Method made before the def. There the call also has the module's
# visibility: a private one refuses an explicit call and public_send, on
# the class side of an extend and on a builtin's reopening too. The same
# holds at the root, for a module Object includes, with a program name
# and a name Kernel has (frozen?), on builtin values, main and program
# objects.
module M
  def w = :m
  def frozen? = :m_frozen
end
class C
  include M
end
class E
  include M
end
def via(o) = o.w
c = C.new
xs = [c, E.new]
p c.w
p via(c)
p [c].map { |x| x.w }
p xs.map { |x| x.w }
p [c.send(:w), c.public_send(:w)]
p c.frozen?
um = c.method(:w)
p um.owner
class C
  def w = :c
  def frozen? = :c_frozen
end
p c.w
p via(C.new)
p [c].map { |x| x.w }
p xs.map { |x| x.w }
p [c.send(:w), c.public_send(:w)]
p c.frozen?
p um.call, c.method(:w).owner

module P
  private def v = :p
end
class D
  include P
  def t = v
end
class K
  extend P
end
d = D.new
begin; p d.v; rescue NoMethodError => e; p e.class; end
begin; p d.public_send(:v); rescue NoMethodError => e; p e.class; end
begin; p K.v; rescue NoMethodError => e; p e.class; end
p d.t, d.send(:v), K.send(:v)
class D
  public def v = :d
end
class K
  def self.v = :k
end
p d.v, d.public_send(:v), d.t, K.v

module Q
  private def zq = :q
end
class Array
  include Q
end
begin; p [].zq; rescue NoMethodError => e; p e.class; end
class Array
  def zq = :arr
end
p [].zq

module N
  def zork = :n
  def frozen? = :n_frozen
end
class Object
  include N
end
class Foo; end
p 1.zork
p "s".frozen?
p zork
p Foo.new.zork
class Object
  def zork = :object
  def frozen? = :object_frozen
end
p 1.zork
p "s".frozen?
p zork
p Foo.new.zork
