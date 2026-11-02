# A class header that reopens a class a builder made (`A::B = Class.new do
# ... end`, then `class A::B`) opens a constant scope of its own, as any
# header does: a class it nests named like a builtin module is a new
# constant under it, not a reopening of the module (TypeError). Only the
# builder's block opens none.
module A; end
A::B = Class.new do
  def y = 0
end
class A::B
  class Enumerable
    def x = 2
  end
end
p A::B::Enumerable.new.x

module C; end
C::D = Class.new do
  def y = 1
end
class C::D < Object
  class Comparable
    def x = 3
  end
end
p C::D::Comparable.new.x, C::D.new.y
p Enumerable.class, Comparable.class
