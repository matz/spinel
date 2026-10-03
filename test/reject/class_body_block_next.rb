# A `next` in the block of Class.new, Module.new, Struct.new or Data.define
# stops the class's body there, so the definitions after it are made or not
# at run time. Which methods a class has is settled at compile time: the
# program is refused, where it used to reach the C compiler with a
# `continue` outside any loop.
Point = Struct.new(:x, :y) do
  next if ARGV.length == 9
  def sum = x + y
end
p Point.new(1, 2).sum
