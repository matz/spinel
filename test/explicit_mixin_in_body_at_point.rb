# K.include(M), K.prepend(M) or K.extend(M) as a statement of a class or
# module body, K's own or another's, is a mixin of K's the load record takes
# where the statement runs (#10790): a call before it does not see it
module Loud
  def hi = "loud " + super
end
module Polite
  def please = "please"
end
module Greet
  def greet = "greet #{name}"
end
class K
  def hi = "k"
  K.include(Polite)
  K.extend(Greet)
end
module Setup
  p K.new.hi
  K.prepend(Loud)
  p K.new.hi
  p K.new.please
  p K.greet
end
p K.ancestors.take(3)
