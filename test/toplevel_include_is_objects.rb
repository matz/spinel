# A top-level `include M` is Object.include(M): every object answers M's
# methods, M is among Object's ancestors, and a bare call still reaches them.
module AM
  def am = :am
  def who = self.class
end
class K; end
include AM
p am
p Object.include?(AM)
p Object.ancestors.include?(AM)
p 1.am
p "s".who
p K.new.am
p [1].who
p nil.am
p K.am
p K.new.is_a?(AM)

# a method of the class's own still comes first
module Greets
  def greet = "module"
  def name_of = "m:" + greet
end
include Greets
class Own
  def greet = "own"
end
p Own.new.greet
p Own.new.name_of
p K.new.name_of
p greet
p name_of

# two modules in one statement, and a module that includes another
module Inner
  def inner = 1
end
module Outer
  include Inner
  def outer = inner + 1
end
module Side
  def side = :side
end
include Outer, Side
p 5.outer
p :sym.side
p Object.include?(Inner)
