# A super into !, nil?, is_a?, kind_of? and instance_of? from a class whose
# objects are by-value (no identity, no class pointer of their own) answers
# as Object's does.
class Pt
  def initialize(x) = @x = x
  def ! = super
  def nil? = super
  def is_a?(k) = super
  def kind_of?(k) = super
  def instance_of?(k) = super
end
q = Pt.new(1)
p q.!, q.nil?
p q.is_a?(Pt), q.is_a?(Object), q.is_a?(Comparable)
p q.kind_of?(Pt), q.kind_of?(String)
p q.instance_of?(Pt), q.instance_of?(Object)
r = Pt.new(2)
p [!r, r.nil?, r.is_a?(Pt)]

# A super into ==, equal?, eql?, frozen? or itself reads the object itself (its
# address, its frozen bit): a class whose method makes one keeps its pointer,
# and the super answers as Object's.
class Qv
  def initialize(x) = @x = x
  def ==(o) = super
  def equal?(o) = super
  def eql?(o) = super(o, &(puts("eql? block"); nil))
  def frozen? = super
  def itself = super
end
v = Qv.new(1)
p v == 1, v == nil, v.equal?(2), v.eql?(nil)
p v.frozen?, v.itself.equal?(3)
w = Qv.new(2)
p [w == 1, w.equal?(nil), w.frozen?]

# != and === reach the object's identity through ==: a class whose method
# makes one keeps its pointer too, and the object equals only itself.
class Qn
  def initialize(x) = @x = x
  def !=(o) = super
  def ===(o) = super
end
n = Qn.new(1)
p n != 1, n != nil, n != n, n === 1, n === nil, n === n
case n
when n then p :same
else p :other
end

# A super into hash is Object's identity hash, never the program's own hash
# again, and a call of hash, object_id or __id__ the class does not answer
# itself reads the object's identity: such a class keeps its pointer, whose
# objects of equal ivars still hash and number apart.
class Hv
  def initialize(x) = @x = x
  def hash = super
end
class Hsub < Hv; end
hs = [Hv.new(1), Hv.new(1), Hv.new(2)]
p hs.map(&:hash).uniq.size, hs[0].hash == hs[0].hash
p Hsub.new(1).hash == Hsub.new(1).hash, [Hsub.new(1), Hsub.new(1)].map(&:hash).uniq.size
class Iv
  def initialize(x) = @x = x
end
a = Iv.new(1)
b = Iv.new(1)
p a.object_id == b.object_id, a.object_id == a.object_id, a.__id__ == b.__id__, a.hash == b.hash, a.hash == a.hash
