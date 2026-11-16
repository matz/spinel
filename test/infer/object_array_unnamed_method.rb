# A method nothing in the program names never runs, so no caller can read
# its value. Its return slot was killed all the same, as a method entered by
# a route the narrowing never saw, and the `select` it answers joined that
# slot to @items: the ivar stayed a boxed poly array, and every read of an
# element went through the boxed dispatch. infer-test checks that @items is
# an sp_PtrArray.
class Item
  attr_reader :id
  def initialize(id) = @id = id
end

class Store
  attr_reader :items
  def initialize = @items = []
  def add(id) = @items << Item.new(id)
  def visible = @items.select { |i| i.id > 1 }
  def hidden = @items.reject { |i| i.id > 1 }
end

s = Store.new
s.add(1)
s.add(2)
puts s.items.sum { |i| i.id }
