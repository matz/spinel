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
