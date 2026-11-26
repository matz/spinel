# spinel: int64
# Each evaluation of a Class.new makes its own class, and every way of
# building an instance builds it for that class: new, allocate, a class
# method's bare new or allocate, one inherited from a static parent, a
# super into the parent's, and a small class of one scalar ivar, which
# keeps the pointer layout.

def mk_alloc = Class.new { def v = 2; def self.make = allocate }
b = mk_alloc
c = mk_alloc
p b.make.class == b, c.make.class == c

class K
  def self.make = new
  def self.me = self
end
def mk_inherit = Class.new(K) { def v = 1 }
a = mk_inherit
d = mk_inherit
p a.make.class == a, d.me == d, [a, d].map { |k| k.make.class == k }, [a, d].map(&:me) == [a, d]

class K2; def self.make = new; end
def mk_super = Class.new(K2) { def self.make = (o = super; o) }
e = mk_super
f = mk_super
p e.make.class == e, f.make.class == f

def mk_plain = Class.new { def v = 1 }
k = mk_plain
j = mk_plain
p k.allocate.class == k, j.allocate.class == j, k.allocate.instance_of?(k)

def mk_value = Class.new { attr_reader :n; def initialize(n) = (@n = n * 3_000_000_000) }
g = mk_value
h = mk_value
o = g.new(4)
p o.n, o.class == g, h.new(2).class == h, o.instance_of?(g), [o].map(&:class) == [g]
