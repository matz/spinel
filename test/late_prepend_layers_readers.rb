# The Method objects and the readers of a late layer answer as the lookup
# does: before its body runs, a Method made of the class's method is the
# one behind the layer, its owner the class's, and a Method's super_method
# passes a layer that is not live; after, the layer's.

module Q1; def self.setup = (def j = [:q1, super]); end
module Q2; def j = [:q2, super]; end
class K1; def j = :k1; end
class K1; prepend Q1; prepend Q2; end
k1 = K1.new
m = k1.method(:j)
p [k1.respond_to?(:j), m.owner, K1.instance_method(:j).owner, m.call, m.super_method.owner, m.super_method.call]
Q1.setup
p [m.owner, m.call, m.super_method.owner, m.super_method.call, m.super_method.super_method.owner]
p [k1.method(:j).owner, k1.method(:j).super_method.owner, k1.method(:j).super_method.call]

# a late layer in front of the class's own def, a Method and a reader of it
module Q2b; def self.setup = (def j = [:q2, super]); end
class K2; prepend Q2b; def j = :k2; end
k2 = K2.new
cap = k2.method(:j)
p [cap.owner, cap.call, K2.instance_method(:j).owner, k2.respond_to?(:j), [k2, 1].map { |x| x.respond_to?(:j) }]
Q2b.setup
p [cap.owner, cap.call, k2.method(:j).owner, k2.method(:j).call, K2.instance_method(:j).owner]

# nothing behind the late layers: a super_method is nil, and behind two or
# more of them it names the live one below each
module Q8; def self.setup = (def j = [:q8]); end
module P8; def j = [:p8, (super rescue :none)]; end
class K8; end
class K8; prepend Q8; end
class K8; prepend P8; end
o8 = K8.new
p o8.method(:j).super_method
Q8.setup
p o8.method(:j).super_method.owner
module L1; def self.setup = (def j = [:l1, super]); end
module L2; def self.setup = (def j = [:l2, super]); end
module L3; def self.setup = (def j = [:l3, super]); end
module P1; def j = [:p1, super]; end
class A; def j = [:a]; end
class A; prepend L1; prepend L2; prepend L3; prepend P1; end
o = A.new
p [o.method(:j).super_method&.owner, o.method(:j).super_method&.super_method&.owner]
L1.setup
p [o.method(:j).super_method&.owner, o.method(:j).super_method&.super_method&.owner]
L3.setup
p [o.method(:j).super_method&.owner, o.method(:j).super_method&.super_method&.owner, o.method(:j).super_method&.super_method&.super_method&.owner]

# the UnboundMethod of a super_method that passes a layer that is not live keeps the method it found: its owner and
# parameters are that method's, not the entry over the layers'
module QU1; def self.setup = (def j(a) = [:q1, a, super(a, 1)]); end
module QU2; def self.setup = (def j(a, b) = [:q2, a, b, super(a)]); end
module QU3; def self.setup = (def j(a, b = 5, *r) = [:q3, a, b, r, super(a, b)]); end
class KU; def j(a, b = 2) = [:k, a, b]; end
class KU; prepend QU1; prepend QU2; prepend QU3; end
QU1.setup
QU3.setup
um = KU.new.method(:j)
usm = um.super_method
p [um.owner, um.parameters, um.unbind.owner, um.unbind.parameters]
p [usm.owner, usm.parameters, usm.unbind.owner, usm.unbind.parameters]
p [usm.super_method.owner, usm.super_method.unbind.owner, usm.super_method.unbind.parameters]
QU2.setup
um = KU.new.method(:j)
usm = um.super_method
p [usm.owner, usm.parameters, usm.unbind.owner, usm.unbind.parameters]
