# A deconstruct no call spells is entered by a pattern, which reads the
# Array it answers boxed: the Array of one class it returns stays a poly
# array and the match takes its elements apart.
class Item
  attr_reader :tag
  def initialize(t) = @tag = t
end

class Box
  def initialize = @items = [Item.new("a"), Item.new("b")]
  def items = @items
  def deconstruct = @items.map { |x| x }
end

b = Box.new
case b
in [x, y] then puts x.tag + y.tag
end
puts b.items.size
