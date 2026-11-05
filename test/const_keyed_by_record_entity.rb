# A constant is named by the entity the load record holds it as (its
# owner and its name), so two constants of one name stay apart where
# CRuby keeps them apart, and a read takes the one CRuby finds.

# a singleton class's constant beside its class's own of the name
class A
  X = 1
  class << self
    X = 2
    def x = X
  end
  def self.y = X
  def y = X
end
p A::X, A.x, A.y, A.new.y

# a singleton class's constant with no class constant of the name
class B
  class << self
    ONLY = :sg
    def only = ONLY
  end
end
p B.only

# a path write inside another module writes the path's constant
module N; end
module M
  N::X = 1
  N::Z = :nz
end
class C; X = 3; def x = X; end
X = 2
p N::X, N::Z, X, C::X, C.new.x

# a read before the class writes its own constant of the name finds the
# outer one
V = 1
class D
  W = V
  V = 2
  def self.z = [W, V]
end
p D.z, V

# reopened: the class's constant stays its own across the bodies
class E; K = :e; end
class F; K = :f; end
class E; def self.k = K; end
p E.k, F::K, E::K
