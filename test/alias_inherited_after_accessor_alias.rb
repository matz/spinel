# An alias of an accessor is a method of its own, `def foo = @foo` under
# the alias's name. A later alias in the same class that gives the
# accessor's name an inherited def (`alias foo x`) is the copy of that def
# all the same: the accessor's method is no later def of the name.
class A
  attr_reader :foo
  attr_writer :bar
  def initialize = @foo = 1
  def x = 42
  def y=(v)
    @y = v * 2
  end
  def y = @y
end
class B < A
  alias old_foo foo
  alias foo x
  alias_method :old_bar=, :bar=
  alias_method :bar=, :y=
  def via = foo
end
b = B.new
p b.old_foo, b.foo, b.via
b.bar = 5
p b.y
b.old_bar = 7
p b.instance_variable_get(:@bar)
[A.new, B.new].each { |o| p o.foo }
