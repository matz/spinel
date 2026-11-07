# A class alias a module or a class holds is a receiver again: `M::Top2.v`
# names the class `Top2` is bound to, though M holds no constant named Top.
# The lookup of a scoped path sees the name the program wrote (the #10577
# answer stays: a scope that holds no such constant does not fall back to the
# top level).
class Top
  def self.v = 1
  def hi = "hi"
end

module M
  Top2 = Top
  A = Top
  B = A
end

class C
  T3 = Top
end

module N
  class In
    def self.v = 2
  end
end

module M
  In2 = N::In
end

p M::Top2.v
x = M::Top2
p x.v
p M::Top2.new.class
p M::Top2.new.hi
p C::T3.v
p M::In2.v
p M::B.v
p M::A.v
p M::Top2.is_a?(Class)

class Holder; end
begin
  Holder::Top.v
rescue NameError => e
  puts e.message
end

# An alias path in a scope that does not hold the alias raises NameError too,
# whichever scope holds it: a class that holds no such constant, a module that
# holds none, and a module nested in the one that holds it.
module M
  module Inner; end
end
module Other; end
begin
  Holder::Top2.v
rescue NameError => e
  p e.class
end
begin
  Other::Top2.new.class
rescue NameError => e
  p e.class
end
begin
  M::Inner::Top2.v
rescue NameError => e
  p e.class
end
