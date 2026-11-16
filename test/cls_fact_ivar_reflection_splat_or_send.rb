# instance_variable_set given its arguments through a splat, or called by a
# send, writes an ivar the analysis cannot name: every ivar may hold it
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
b = Box.new
b.instance_variable_set(:@v, B.new)
p b.v.hi
c = Box.new
c.instance_variable_set(:@v, *[A.new])
p c.v.hi
d = Box.new
d.send(:instance_variable_set, :@v, A.new)
p d.v.hi
