# A prepended module's versions of a method are versions in the class: a
# super in either reaches the class's own method, not the module's earlier
# version.
module V1
  def a = [super, 1]
end
class K1; def a = :own; prepend V1; end
p K1.new.a
module V1; def a = [super, 2]; end
p K1.new.a

module V2
  def a = [super, 1]
  def a = [super, 2]
end
class K2; def a = :own; prepend V2; end
p K2.new.a

# a prepend a reopening runs after the class's first body
module V3
  def a = [super, 1]
  def a = [super, 2]
end
class K3; def a = :own; end
k = K3.new
p k.a
class K3; prepend V3; end
p k.a
