# An alias over an inherited accessor's name is a method of the aliasing
# class, so a subclass of that class reaches the alias's method, not the
# accessor: an alias of an ancestor's def, of the class's own def or of a
# builtin's method, a writer alias, and calls through self and send.
class A
  attr_accessor :foo
  def initialize; @foo = 1; end
  def x = 42
  def y=(v); @y = v; end
  attr_reader :y
end

class B < A
  alias foo x
end
class C < B; end
p C.new.foo, B.new.foo
[A.new, B.new, C.new].each { |o| p o.foo }

class WB < A
  alias foo= y=
end
class WC < WB; end
w = WC.new
w.foo = 9
p w.foo, w.y

class SB < A
  alias_method :foo, :x
  def via = foo
end
class SC < SB
  def via2 = foo
end
p SC.new.via, SC.new.via2, SC.new.send(:foo), SC.new.method(:foo).call

class OB < A
  def z = 7
  alias foo z
end
class OC < OB; end
p OC.new.foo

class IB < A
  alias foo inspect
end
class IC < IB; end
p IC.new.foo.start_with?("#<IC")

class CB < A
  alias foo x
end
class CC < CB
  def foo = super + 1
end
class CD < CB
  attr_reader :foo
end
p CC.new.foo, CD.new.foo
