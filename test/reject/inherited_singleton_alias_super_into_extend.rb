# A class-side alias of an inherited class method whose super reaches a module
# the class extends, behind a later def of the name, has no method of that body
# to start its super from: refused.
module Base
  def m = [:base]
end
class A
  extend Base
  def self.m = [:a, *super]
end
class B < A
  class << self
    alias n m
  end
end
class A
  def self.m = [:a2]
end
p B.n, B.m
