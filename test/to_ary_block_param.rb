# A to_ary that takes a block parameter it never uses (`def to_ary(&) = ...`,
# as a record class delegating its rows writes it) is the conversion
# protocol's to_ary all the same: no positional parameter. Its C function
# takes the block as its last parameter, and the conversion switch called it
# with the object alone: the C did not build. The switch passes no block.
class Row
  def initialize(*cells) = @cells = cells
  def to_ary(&) = @cells.dup
  def to_s = "row#{@cells}"
end
class Pair
  def initialize(a, b) = (@a, @b = a, b)
  def to_ary = [@a, @b]
end
x, y, z = Row.new(1, 2, 3)
p [x, y, z]
first, second = Pair.new("a", "b")
p [first, second]
p [Row.new(4), [5, Row.new(6, 7)]].flatten
p Array(Row.new(8, 9))
[Row.new(10, 11), Pair.new(12, 13)].each { |(a, b)| p [a, b] }
