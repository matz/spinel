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
