# A late include of a module that defines the method again, then a late
# include of another module of the name: a call between them runs the
# version the first module holds then, and the second module's super
# reaches the first module's version live when it runs.
module V1; def a = 1; end
module W1; def a = [:w, super]; end
class P1; def a = :p; end
class K1 < P1; end
p K1.new.a
class K1; include V1; end
p K1.new.a
module V1; def a = 2; end
p K1.new.a
class K1; include W1; end
p K1.new.a

# both modules define the method again
module V2; def a = [:v1, super]; end
module W2; def a = [:w1, super]; end
class P2; def a = :p; end
class K2 < P2; end
p K2.new.a
class K2; include V2; end
p K2.new.a
module V2; def a = [:v2, super]; end
p K2.new.a
class K2; include W2; end
p K2.new.a
module W2; def a = [:w2, super]; end
p K2.new.a

# a late prepend in front of the class's own method
module V3; def a = [:v1, super]; end
class K3; def a = :k; end
k3 = K3.new
p k3.a
module V3; def a = [:v2, super]; end
p k3.a
class K3; prepend V3; end
p k3.a
module V3; def a = [:v3, super]; end
p k3.a
