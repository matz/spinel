# A class nested in a header that reopens a builder's class (`A::B = Class.new
# {}`, then `class A::B`) is named under that class and found from its
# methods, as any header's: the header opens a constant scope of its own,
# which the class's definition (the builder, as `class A::B < Object`
# leaves it) does not tell. Only a builder's block opens none: a class
# written in it is named in the scope around the write.
module A; end
A::B = Class.new {}
class A::B < Object
  class Inner
    def v = 7
    class Deep; def d = 9; end
  end
  W = Inner
  def mk = Inner.new
  def deep = Inner::Deep.new
  def self.cls = Inner
end
p A::B::Inner.name, A::B.new.mk.class, A::B.new.deep.class, A::B.cls
p A::B::W, A::B::Inner::Deep.name

module A2; end
A2::E = Class.new { def y = 0 }
class A2::E
  class Inner2; end
  module Mod2; X = 2; end
end
p A2::E::Inner2.name, A2::E::Mod2::X

# the builder's block, also one nested in another's, and an anonymous one
Out = Class.new do
  class InBlock; end
  Nested = Class.new do
    class InNested; end
  end
end
k = Class.new do
  class InAnon; def v = 1; end
  def m = InAnon.new.v
end
p InBlock.name, InNested.name, InAnon.name, k.new.m
p A.constants, Out.constants.sort
