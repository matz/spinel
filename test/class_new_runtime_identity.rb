# A Class.new evaluated more than once makes a class of its own each time,
# as CRuby does: identity, name, display, superclass, frozen state, and the
# class its instances answer (inside initialize too).
class Base
  attr_reader :seen
  def initialize = (@seen = self.class)
end

def make = Class.new(Base) { def hi = 1 }

a = make
b = make
p a == b, a.equal?(a), a.superclass == Base, b.superclass == Base
p a.name, a.to_s.start_with?("#<Class:0x"), a.to_s == b.to_s
Named = a
p a.name, a.to_s, Named.new.class == a, b.name
x = a.new
y = b.new
p x.class == a, y.class == b, x.class == y.class, x.seen == a, y.seen == b
p x.is_a?(Base), x.is_a?(a), x.is_a?(b), x.hi + y.hi
p x.inspect.start_with?("#<Named"), y.inspect.start_with?("#<#<Class:0x")
h = { a => :a, b => :b }
p h[x.class], h[y.class], h.size
r = [x, y].map do |o|
  case o
  when a then :is_a
  when b then :is_b
  else :none
  end
end
p r
b.freeze
p a.frozen?, b.frozen?
ks = 3.times.map { make }
p ks.uniq.size, ks.map(&:superclass).uniq == [Base]
# a small class of scalar ivars keeps its own class in its methods
mkv = -> { Class.new { def initialize(n) = (@n = n); def n = @n; def kl = self.class; def nxt = self.class.new(@n + 1); def own?(k) = instance_of?(k) } }
va = mkv.call
vb = mkv.call
p va.new(1).kl == va, vb.new(1).kl == vb, va.new(1).own?(vb), vb.new(2).nxt.n, vb.new(2).nxt.class == vb
