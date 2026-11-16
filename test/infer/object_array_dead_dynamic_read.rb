# A method nothing in the program names never runs, so a call in it reaches
# no object. One that calls a reader on an untyped parameter (its parameter
# has no type when no call passes it one) was a dynamic receiver all the
# same, which may be any class with that reader: it kept the ivar of every
# such class a boxed poly array, and every read of an element went through
# the boxed dispatch. infer-test checks that @items is an sp_PtrArray. A
# method a Symbol or a String names, or that the host calls, still counts.
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
end

module Report
  def self.big(store) = store.items.select { |i| i.id > 1 }
end

def big_items(store) = store.items.select { |i| i.id > 1 }

s = Store.new
s.add(1)
s.add(2)
puts s.items.sum { |i| i.id }
