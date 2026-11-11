# An alias of an inherited class method whose `super` reaches a module the class
# extends, behind a later definition of its name, with two modules extended, is
# refused as with one.
module M1
  def m = :m1
end
module M2
  def m = [:m2, super]
end
class A
  extend M1
  extend M2
end
class B < A
  class << self
    alias n m
  end
end
class A
  def self.m = :redef
end
p A.m, B.m, B.n
