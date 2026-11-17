# instance_variable_set / instance_variable_get with a literal name read and
# write that ivar alone: the program's other ivars keep their classes, and
# what the literal write stores is in the ivar it names
class Box
  def initialize = @v = nil
  def v = @v
end
class A
  def hi = "A"
end
class B
  def hi = "B"
end
class Holder
  def initialize(x) = @x = x
  def x = @x
end
h = Holder.new(A.new)
b = Box.new
b.instance_variable_set(:@v, B.new)
p b.v.hi
p b.instance_variable_get(:@v).hi
p h.x.hi
c = Box.new
c.instance_variable_set("@v", A.new)
p c.v.hi
