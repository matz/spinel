# super in a module's method reaches the module that module includes, for
# any class the outer module is mixed into: included at depth two and three,
# under a class's own override, with arguments and a block, prepended into a
# class, extended into one, and mixed in after the inner module or before an
# include of it, which leaves the ancestors as they were.

module Base
  def greet = "base"
end

module Mid
  include Base
  def greet = "mid+" + super
end

module Top
  include Mid
  def greet = "top+" + super
end

module Calc
  def calc(x, y = 1) = x * 10 + y
  def each_two
    yield 1
    yield 2
  end
end

module CalcPlus
  include Calc
  def calc(x, y = 2) = super + 1000
  def each_two
    r = []
    super { |v| r << v * 3 }
    r
  end
end

class Plain
  include Mid
  include CalcPlus
end

class Deep
  include Top
end

class Parent
  def greet = "parent"
end

class Child < Parent
  include Mid
  def greet = "child+" + super
end

class Front
  prepend Mid
  def greet = "front"
end

class Ext
  extend Mid
end

class Twice
  include Mid
  include Base
end

class Before
  include Base
  include Mid
end

# a module the class mixed in before, or one such a module brings, is
# mixed in once: a repeat leaves the ancestors as they are
module Left
  include Base
  def greet = "left+" + super
end

module Right
  include Base
  def greet = "right+" + super
end

module Again
  include Mid
end

class Repeated
  include Mid
  include Mid
end

class Through
  include Mid
  include Again
end

class PrependBoth
  prepend Left
  prepend Right
  def greet = "own"
end

class PrependInner
  prepend Mid
  prepend Base
  def greet = "own"
end

class PrependThenInclude
  prepend Mid
  include Base
  def greet = "own+" + super
end

class Upper
  include Mid
end

class Lower < Upper
  include Mid
end

# an inner module the ancestors hold already, through a superclass or a
# prepend, is not copied again: Mid's super reaches what the class's does
module Chained
  def greet = "chained+" + super
end

module Outer
  include Chained
  def greet = "outer+" + super
end

class Root
  def greet = "root"
end

class HasChained < Root
  include Chained
end

class BelowChained < HasChained
  include Outer
end

class PrependsChained < Root
  prepend Chained
  include Outer
  def greet = "own+" + super
end

class ExtendBoth
  extend Left
  extend Right
end

class ExtendInner
  extend Mid
  extend Base
end

p Plain.new.greet
p Plain.new.calc(3)
p Plain.new.calc(3, 4)
p Plain.new.each_two
p Deep.new.greet
p Child.new.greet
p Front.new.greet
p Ext.greet
p Twice.new.greet
p Before.new.greet
p Repeated.new.greet
p Through.new.greet
p PrependBoth.new.greet
p PrependInner.new.greet
p PrependThenInclude.new.greet
p ExtendBoth.greet
p ExtendInner.greet
p Lower.new.greet
p BelowChained.new.greet
p PrependsChained.new.greet
