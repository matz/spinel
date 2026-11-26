# a class method that collects up the chain through superclass, beside a
# class method that memoizes into a class-level ivar
class Base
  def self.own = (@own ||= ["own"])
  def self.rules = self == Base ? own : superclass.rules + own
end

class Child < Base
end

p Child.rules

# through self.superclass
class Root
  def self.own = (@own ||= ["root"])
  def self.rules = self == Root ? own : self.superclass.rules + own
end

class Leaf < Root
end

p Leaf.rules

# a chain of three classes
class A
  def self.own = (@own ||= [name])
  def self.rules = self == A ? own : superclass.rules + own
end

class B < A; end
class C < B; end

p C.rules, B.rules
