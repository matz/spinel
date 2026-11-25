# instance_methods(false) and public_instance_methods(false) with no
# receiver or on self in a class method list the receiving class's own
# methods.
class T
  def self.own = public_instance_methods(false)
  def self.own_all = instance_methods(false)
  def self.own_self = self.public_instance_methods(false)
  def test_a = 1

  protected def guarded = 2

  private def hidden = 3
end

p T.own
p T.own_all.sort
p T.own_self

# an inherited class method lists the class that received the call
class Base
  def self.own = public_instance_methods(false)
  def base_m = 0
end

class Leaf < Base
  def leaf_m = 1
end

class Bare < Base
end

p Base.own
p Leaf.own
p Bare.own
[Leaf, Base].each { |k| p k.own }

# a module that two unrelated classes extend
module Lister
  def own = instance_methods(false).sort
end

class One
  extend Lister
  def one_a = 1
  def one_b = 2
end

class Two
  extend Lister
  def two_a = 1
end

p One.own
p Two.own

# a `class << self` def in a class with no class below it
class Alone
  class << self
    def own = instance_methods(false).sort
  end
  def alone_a = 1
end

p Alone.own

# a class method that yields the list runs on the class that received the call
class Yielder
  class << self
    def each_own
      yield instance_methods(false).sort
    end
  end
  def y1 = 1
end

class YieldLeaf < Yielder
  def y2 = 2
end

Yielder.each_own { |l| p l }
YieldLeaf.each_own { |l| p l }

# a second class that defines one of the listed names, initialize included
class Holder
  def initialize(x) = @x = x
  def item = @x
  def self.list = instance_methods(false)
end

class Other
  def initialize = nil
  def item = 1
end

p Holder.list
p Other.new.item

# a test runner's base class, with run defined again in a subclass
class Runner
  def run = 0
  def self.tests = public_instance_methods(false).sort
end

class CaseA < Runner
  def run = 1
  def test_x = 1
end

class CaseB < Runner
  def test_y = 1
end

p CaseA.tests
p CaseB.tests

# an Enumerable class with a yielding each
class Bag
  include Enumerable
  def each
    yield 1
    yield 2
  end
  def self.own = public_instance_methods(false).sort
end

p Bag.own
p Bag.new.to_a
