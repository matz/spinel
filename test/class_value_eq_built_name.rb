# spinel: gc-stress
# Two class values held boxed compare by name (==, a Hash key's eql?,
# equal?), and a singleton class held by its id renders its name
# (#<Class:C>) when compared, an allocation: the first name is rooted while
# the second is built. It was not, so under
# SPINEL_GC_STRESS=2 the first was freed and poisoned and a singleton class
# compared unequal to itself.
class C
  def self.a = 1
  def b = 2
end
class D < C; end

ms = [C.method(:a), C.new.method(:b), D.method(:a)]
p ms.map { |m| m.owner == C.singleton_class }
p ms[0].owner == C.singleton_class, ms[2].owner == C.singleton_class
p ms.map(&:owner).uniq.size

vals = [C, C.singleton_class, D, D.singleton_class, String]
p vals.map { |v| vals.count { |w| w == v } }
r = 0
20.times { r += 1 if C.singleton_class == C.method(:a).owner }
p r
p [C.singleton_class, D.singleton_class].include?(D.singleton_class)

# as a Hash key, and by identity
def boxed(x) = x.respond_to?(:zzz) ? 0 : x
h = { boxed(C.singleton_class) => 1, boxed(D.singleton_class) => 2 }
r = []
10.times { r << h[boxed(C.singleton_class)] << h[boxed(D.singleton_class)] }
p r.uniq, h.size
e = []
10.times { e << boxed(C.singleton_class).equal?(boxed(C.singleton_class)) }
p e.uniq
k = [boxed(C.singleton_class), boxed(D.singleton_class), boxed(C)]
p k.uniq.size, k.count(boxed(C.singleton_class))
