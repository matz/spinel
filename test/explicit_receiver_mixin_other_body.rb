# `K.include(M)` / `K.prepend(M)` in another class's or module's body: the
# load record takes the mixin as K's, where the statement runs (#10790).
module M; def hi = :m; end
class K; end
module N; K.include(M); end
p K.new.hi, K.include?(M)

# in the class's own body, and a core module from another module
class K2; K2.include(M); end
module N2; K2.include(Comparable); end
p K2.new.hi, K2.include?(Comparable)

class Ver
  attr_reader :v
  def initialize(v) = @v = v
  def <=>(o) = v <=> o.v
end
module Setup; Ver.include(Comparable); end
p Ver.new(1) < Ver.new(2), [Ver.new(3), Ver.new(1)].min.v

# several modules at once, from a class body; and a prepend
module A; def a = :a; end
module B; def b = :b; end
class Host; end
class Other
  Host.include(A, B)
  def self.go = [Host.new.a, Host.new.b]
end
p Other.go, Host.ancestors.take(3)

module Loud; def say = super.upcase; end
class Speaker; def say = "hi"; end
module Wiring; Speaker.prepend(Loud); end
p Speaker.new.say, Speaker.ancestors.first
