# A module holds one method of a name at a time: a def it makes again (in
# a reopening, or once more in the same body) replaces the earlier one, so
# a class that includes it sees the versions in turn, and a super in
# either reaches past the module, not the module's earlier version.
module V1; def a = 1; end
class P1; def a = :p; end
class K1 < P1; include V1; end
p K1.new.a
module V1; def a = [super, 2]; end
p K1.new.a

module V2
  def a = 1
  def a = [super, 2]
end
class P2; def a = :p; end
class K2 < P2; include V2; end
p K2.new.a

# behind another module the class included before
module W3; def a = :w; end
module V3; def a = [super, 1]; end
class K3; include W3; include V3; end
p K3.new.a
module V3; def a = [super, 2]; end
p K3.new.a

# the earlier version calls no super, the later one does
module W4; def a = :w; end
module V4; def a = 1; end
class K4; include W4; include V4; end
p K4.new.a
module V4; def a = [super, 2]; end
p K4.new.a

# a call in another of the module's methods meets each version
module V5
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
class K5; include V5; end
p K5.new.b(3)
module V5; def a(x) = 50; end
p K5.new.b(3)
