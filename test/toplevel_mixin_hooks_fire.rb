# A top-level K.include(M), K.prepend(M) or K.extend(M) fires M's hook where
# the statement runs (#10789): the load record takes the statement as a
# mixin of K's and the walk fires the hook after it, as after a body's own
# include; a hook defined later fires only for the mixins after it
module Speak
  def self.included(base) = puts("included in #{base}")
  def hello = "hello"
end
module Loud
  def self.prepended(base) = puts("prepended to #{base}")
  def hello = "LOUD " + super
end
module Shout
  def self.extended(base) = puts("extended #{base}")
  def shout = "HEY"
end
module Late
  def late = :late
end
class Console
end
puts "before"
Console.include(Speak)
Console.include(Late)
module Late
  def self.included(base) = puts("late hook for #{base}")
end
class Other
end
Other.include(Late)
Console.prepend(Loud)
Console.extend(Shout)
puts "after"
p Console.new.hello, Console.shout, Console.new.late, Other.new.late
