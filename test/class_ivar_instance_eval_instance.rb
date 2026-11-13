# An instance_eval block on an instance writes and reads that instance's
# ivar, which is not the class's own ivar of the same name, at the top
# level and in a method.
class A
  @x = "class"
  @y = "class y"
  def self.x = @x
  def self.y = @y
  def x = @x
end
class B
  def peek(a) = a.instance_eval { @y }
end
a = A.new
a.instance_eval { @x = 5 }
p a.instance_variable_get(:@x)
p a.x + 1
p a.instance_variables
p A.x
p A.new.x
p a.instance_eval { @y }
p B.new.peek(a)
p A.y
