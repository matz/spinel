# `super` in a class-level `include?` override reaches Module#include? when no
# ancestor of the program defines one, for the receiving class.
module R; end
module Q; end

class B
  include R
  include Q
end

class A < B
  def self.include?(mod) = mod == R ? false : super
end

class C < A; end

class D < B
  def self.include?(mod)
    return true if mod == Comparable
    super(mod)
  end
end

def has(c, mod) = c.class.include?(mod)

p has(C.new, R)
p has(B.new, R)
p C.include?(Q)
p A.include?(Q)
p A.include?(Comparable)
p D.include?(Comparable)
p D.include?(R)
p has(D.new, Q)
