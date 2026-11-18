# A parameter that the first statement of the body overwrites with
# `obj.method(:sym)`, before anything reads it, holds that method: its
# argument reaches no read. Each use of the local answers for the method
# its write names, `super_method` and `receiver` among them, whatever
# Method the caller passed.
class Parent
  def f(x) = [:parent_f, x]
end
class Child < Parent
  def f(x) = [:child_f, x]
end
class OtherParent
  def g(x) = [:other_parent_g, x]
end
class Other < OtherParent
  def g(x) = [:other_g, x]
end
CHILD = Child.new
OTHER = Other.new
def other = OTHER.method(:g)
def use0(m)
  m = CHILD.method(:f)
  m.call(1)
end
def use1(m)
  m = CHILD.method(:f)
  m[2]
end
def use2(m)
  m = CHILD.method(:f)
  m === 3
end
def use3(m)
  m = CHILD.method(:f)
  m.owner
end
def use4(m)
  m = CHILD.method(:f)
  m.name
end
def use5(m)
  m = CHILD.method(:f)
  m.original_name
end
def use6(m)
  m = CHILD.method(:f)
  m.arity
end
def use7(m)
  m = CHILD.method(:f)
  m.parameters
end
def use8(m)
  m = CHILD.method(:f)
  m.to_proc.call(4)
end
def use9(m)
  m = CHILD.method(:f)
  m.to_proc.arity
end
def use10(m)
  m = CHILD.method(:f)
  m.curry[5]
end
def use11(m)
  m = CHILD.method(:f)
  [m == CHILD.method(:f), m == OTHER.method(:g)]
end
def use12(m)
  m = CHILD.method(:f)
  m.source_location[1]
end
def use13(m)
  m = CHILD.method(:f)
  [6, 7].map(&m)
end
def use14(m)
  m = CHILD.method(:f)
  m.dup.call(8)
end
def use15(m)
  m = CHILD.method(:f)
  m.clone.call(9)
end
def use16(m)
  m = CHILD.method(:f)
  m.unbind.name
end
def use17(m)
  m = CHILD.method(:f)
  m.receiver.class
end
def use18(m)
  m = CHILD.method(:f)
  m.unbind.bind(m.receiver).call(10)
end
def use19(m)
  m = CHILD.method(:f)
  m.super_method.call(11)
end
def use20(m)
  m = CHILD.method(:f)
  m.super_method.owner
end
def boxed(m, value)
  m = CHILD.method(:f)
  m.call(value)
end
p use0(other)
p use1(other)
p use2(other)
p use3(other)
p use4(other)
p use5(other)
p use6(other)
p use7(other)
p use8(other)
p use9(other)
p use10(other)
p use11(other)
p use12(other)
p use13(other)
p use14(other)
p use15(other)
p use16(other)
p use17(other)
p use18(other)
p use19(other)
p use20(other)
[1.5, "s", nil].each { |value| p boxed(other, value) }
