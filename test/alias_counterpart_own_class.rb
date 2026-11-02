# An alias keeps its counterpart live in its own class: a singleton alias
# its class's method of the name (an inherited one too, and through an
# alias of an alias), an instance alias its target, and neither the methods
# of that name in other classes nor any method of the alias's name.
module M0
  def self.f0(x) = x * 1
  def self.f1(x) = x * 2
  class << self
    alias g0 f0
    alias g1 f1
  end
end
module M1
  def self.f0(x) = x * 3
  def self.f1(x) = x * 4
  class << self
    alias g0 f0
    alias g1 f1
  end
end
p M0.g0(1), M1.g1(1)

class A
  def self.f(x) = x + 1
end
class B < A
  class << self
    alias g f
    alias h g
  end
end
p B.h(1), B.g(2)

class K
  def f(x) = x + 1
  alias g f
  alias h g
end
class L
  def f(x) = x * 3
  alias g f
end
p K.new.h(1), L.new.f(2), K.new.send(:g, 5)
