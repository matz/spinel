# `class A::B` puts only A::B in the body's lexical scope, not A: CRuby raises
# NameError (uninitialized constant A::B::LIMIT) where spinel read A::LIMIT.
module A
  LIMIT = 3
end
class A::B
  def self.limit = LIMIT
end
p A::B.limit
