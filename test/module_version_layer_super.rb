# A super from a later mixin's method reaches the method an earlier
# module holds when the super runs: the module's versions of it are told
# apart by the module's cells, not bound to the last one.
module A1; def a = :a; end
module V1; def a = [:v1, super]; end
module W1; def a = [:w, super]; end
class K1; include A1; include V1; include W1; end
p K1.new.a
module V1; def a = [:v2, super]; end
p K1.new.a

# a later include: until it runs, a call reaches the earlier module's
# version live then
module V2; def a = [:v, super]; end
module W2; def a = [:w, super]; end
class P2; def a = :k; end
class K2 < P2; include V2; end
p K2.new.a
class K2; include W2; end
p K2.new.a
module V2; def a = [:v2, super]; end
p K2.new.a

# a later prepend over a prepend in the class's first body
module V3; def a = [:v, super]; end
module W3; def a = [:w, super]; end
class K3; def a = :k; prepend V3; end
p K3.new.a
class K3; prepend W3; end
p K3.new.a
module V3; def a = [:v2, super]; end
p K3.new.a

# arguments and a block pass on through the super
module V4; def a(x) = [:v, x, yield]; end
module W4; def a(x) = [:w, super(x + 1)]; end
class K4; include V4; include W4; end
p K4.new.a(1) { :b }
module V4; def a(x) = [:v2, x, yield]; end
p K4.new.a(1) { :c }

# an earlier version that yields gets the block the later layer's super
# hands on, also where the last version does not yield
module V5; def a = [:v1, yield]; end
module W5; def a = [:w, super]; end
class K5; include V5; include W5; end
p K5.new.a { 5 }
module V5; def a = [:v2]; end
p K5.new.a { 5 }

# ...and so through a later include's copy
module V6; def a = [:v1, yield]; end
module W6; def a = [:w, super]; end
class K6; include V6; end
p K6.new.a { 5 }
class K6; include W6; end
p K6.new.a { 5 }
module V6; def a = [:v2]; end
p K6.new.a { 5 }

# two later mixins: the earlier one has always run when the later one's
# super does, which reaches the version its module holds then
module V7; def a = [:v1, super]; end
module W7; def a = [:w, super]; end
class P7; def a = :p; end
class K7 < P7; end
class K7; include V7; end
class K7; include W7; end
p K7.new.a
module V7; def a = [:v2, super]; end
p K7.new.a

module V8; def a = [:v1, super]; end
module W8; def a = [:w, super]; end
class K8; def a = :k; end
p K8.new.a
class K8; prepend V8; end
p K8.new.a
class K8; prepend W8; end
p K8.new.a
module V8; def a = [:v2, super]; end
p K8.new.a
