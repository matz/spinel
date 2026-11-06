# A program's own step takes its keyword arguments as written; only
# Numeric#step's keyword forms are lowered to positional arguments
class Foo
  def step(to: 0, by: 1) = "foo #{to} #{by}"
end
puts Foo.new.step(by: 3)
puts Foo.new.step(to: 5, by: 2)
puts Foo.new.step(by: 2, to: 7)
puts Foo.new.step(to: 9)
class Bar < Foo; end
puts Bar.new.step(by: 4)
f = Foo.new
p 1.step(by: 3).first(3)
p 1.step(to: 10, by: 4).to_a
x = 2
p x.step(10, by: 3).to_a
class Baz
  def step(*a, **kw) = [a, kw]
end
p Baz.new.step(by: 3)
p Baz.new.step(5, by: 3)
module Stepper
  def step(by: 1) = "mod #{by}"
end
class Q; include Stepper; end
p Q.new.step(by: 6)

# receivers typed only later: a method's result, an ivar, a block parameter
def mk = Foo.new
puts mk.step(to: 3, by: 3)
class Holder
  def initialize = @f = Foo.new
  def run = @f.step(to: 7, by: 1)
end
puts Holder.new.run
[Foo.new].each { |x| puts x.step(by: 6) }

# a class method named step
class KS
  def self.step(to: 0, by: 1) = "KS.step #{to} #{by}"
end
puts KS.step(to: 2, by: 3)
