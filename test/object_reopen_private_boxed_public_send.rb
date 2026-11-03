# A private (or protected) method of a reopened Object, reached by
# public_send or through a boxed receiver, raises NoMethodError for every
# value whose class does not answer the name itself.
class Object
  private def top = "t"
  protected def pro = "p"
end
module M; def top = "m"; end
class D; def top = "own"; end
class E < D; end
class F; include M; end
class G; attr_reader :top; def initialize = @top = "g"; end
S = Struct.new(:top)
class I; end

def try
  yield
rescue NoMethodError => e
  "NME #{e.message}"
end

[D.new, E.new, F.new, G.new, S.new("s"), I.new, "x", 3, nil, :sym, [1], Object.new].each do |x|
  p [try { x.top }, try { x.public_send(:top) }, x.send(:top)]
end

p try { I.new.public_send(:top) }
p try { 5.public_send(:top) }
p try { Object.new.public_send(:top) }
p try { Object.new.top }
p [G.new.public_send(:top), S.new(2).public_send(:top), E.new.public_send(:top)]

class K
  def a = public_send(:top)
  def b = top
  def c = self.public_send(:top)
  def d(o) = o.pro
  def e(o) = o.public_send(:pro)
  def self.cm = 3.pro
end
k = K.new
p [try { k.a }, k.b, try { k.c }, k.d(3), try { k.e(3) }, K.cm]
p 3.pro
p try { public_send(:top) }

class BO < BasicObject
  def go = 3.pro
end
p try { BO.new.go }

# a builtin subclass, a class method, a visibility a class declares, and
# a safe call on nil answer as CRuby does
class AS < Array; def top = "as"; end
class HS < Hash; def top = "hs"; end
class CM; def self.top = "ctop"; end
class P; public :top; end
y = nil
[AS.new, HS.new, P.new, CM, 7].each do |x|
  p [try { x.top }, try { x.public_send(:top) }]
end
p [CM.public_send(:top), P.new.public_send(:top), y&.public_send(:top)]

class KC
  def self.b = public_send(:top)
end
p try { KC.b }

# a Method calls its method whatever its visibility
class R; private def sec = "sec"; end
p [I.new, D.new].map { |x| x.method(:top).call }
p [R.new, 1].first.method(:sec).call
