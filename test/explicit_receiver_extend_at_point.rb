# K.extend(M) at the top level, M a module of the program, is a mixin of
# K's singleton the load record takes where the statement runs (#10790):
# a call between two extends sees the first alone
module Speak
  def hello = "speak"
  def who = "speak #{name}"
end
module Loud
  def hello = "loud"
end
class Console
  def self.own = "own"
end
module Store
end
Console.extend(Speak)
p Console.hello
p Console.who
Console.extend(Loud)
p Console.hello
p Console.who
p Console.own
Store.extend(Speak, Loud)
p Store.hello
p Store.who
p Console.is_a?(Loud), Store.is_a?(Speak)
