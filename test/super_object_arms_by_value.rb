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
