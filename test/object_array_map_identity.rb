# A map whose block answers its element (`as.map { |a| a }`) over an Array
# of one class builds a new array of the same elements, as `as.map(&:dup)`
# builds one of their copies. It was no source the narrowing modeled, so
# writing it back (`@items = @items.map { |i| i }`) kept the array a boxed
# poly array, and every read of an element went through the boxed
# dispatch. infer-test checks that @items is an sp_PtrArray.
class Item
  attr_reader :id
  def initialize(id) = @id = id
end

class Store
  attr_reader :items
  def initialize = @items = []

  def add(id)
    @items << Item.new(id)
    nil
  end

  def rebuild
    @items = @items.map { |i| i }
    nil
  end

  def copy = @items.map { |i| i }

  def grown
    list = @items.map { |i| i }
    list << Item.new(7)
    list.map(&:id)
  end
end

s = Store.new
s.add(1)
s.add(2)
s.add(3)
s.rebuild
p s.items.map(&:id)
c = s.copy
c << Item.new(9)
p [c.size, s.items.size]
p c.map(&:id)
p s.grown
p s.items.map(&:id)
