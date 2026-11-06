# A mixin on a builtin class or module through an explicit receiver at the
# top level runs where the statement is (#10790): the load record takes it
# as one of the class's mixins, after an empty reopening gives it the class
module Twice
  def twice = self * 2
end
module Loud
  def shout = to_s.upcase + "!"
  def size = super + 100
end
module MathExt
  def half(x) = x / 2.0
end
module Tag
  def tag = "tagged"
end

Integer.include(Twice)
String.prepend(Loud)
p 3.twice
p "abc".shout
p "abc".size
p Integer.include?(Twice)
::Math.singleton_class.prepend(MathExt)
p Math.half(5)
p Math.sqrt(16)
Array.send(:include, Tag)
p [1, 2].tag
p [Integer < Twice, String < Loud]
