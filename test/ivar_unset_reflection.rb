# An ivar a class can hold but nothing has assigned yet is not one of the
# object's instance variables, as in CRuby: instance_variables leaves it
# out, instance_variable_defined? and defined?(@x) answer false and nil,
# and the default inspect does not show it. Each answered from the class's
# layout, as if every ivar the class ever writes were set from the start.

class A
  def initialize = (@a = 1)
  def s = (@x = 2)
  def d = defined?(@x)
end
class B < A
  def initialize = (super; @b = "b")
  def t = (@y ||= [1])
end
class C
  attr_writer :w
  def initialize(flag) = (@c = 0; @z = "z" if flag)
  def z? = instance_variable_defined?(:@z)
end

b = B.new
p b.instance_variables.sort, b.instance_variable_defined?(:@y), b.d
puts b.inspect.sub(/0x\h+/, "0x")
b.t; b.s
p b.instance_variables.sort, b.instance_variable_defined?(:@y), b.d
c = C.new(false); c2 = C.new(true)
p c.instance_variables, c.z?, c2.instance_variables, c2.z?
c.w = "w"; p c.instance_variables
xs = [A.new, B.new, 1]
p xs.map { |o| o.instance_variables.sort }
p xs.map { |o| o.instance_variable_defined?(:@x) }
xs[0].s
p xs.map { |o| o.instance_variable_defined?(:@x) }
a0 = A.new
puts a0.inspect.sub(/0x\h+/, "0x")
