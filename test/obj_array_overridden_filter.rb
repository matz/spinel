# The program's own delete_if, keep_if, sort_by! and reject on Array answer
# what their bodies say, not the receiver nor a copy of its elements. An
# Array of one class whose filter value is read through such a method stays
# boxed: the filter arm's edge to the receiver's slot typed the method that
# returns the value an sp_PtrArray over a body inferred boxed, and spinel
# refused the program (for delete_if, keep_if, sort! and sort_by! since they
# took that edge; for reject, select and sort_by before). Where the value is
# thrown away (Tidy) the Array stays boxed too: the program's select! stores
# a String in it, which an Array narrowed to Item raised TypeError on.
module Sorted
  def sort_by! = [:sorted, 1]
end

class Array
  prepend Sorted
  def delete_if = [1, "two", :three]
  def keep_if = []
  def reject = ["r", 2.5]
  def select!
    self << "intruder"
    nil
  end
end

class Item
  attr_reader :name
  def initialize(name) = @name = name
end

class Shelf
  def initialize = @items = [Item.new("a"), Item.new("b")]
  def drop = @items.delete_if { |i| i.name == "b" }
  def first_name = @items[0].name
end

class Bin
  def initialize = @items = [Item.new("c"), Item.new("d")]
  def keep = @items.keep_if { |i| i.name == "c" }
  def first_name = @items[0].name
end

class Queue2
  def initialize = @items = [Item.new("e"), Item.new("f")]
  def by_name = @items.sort_by! { |i| i.name }
  def first_name = @items[0].name
end

class Pick
  def initialize = @items = [Item.new("g"), Item.new("h")]
  def others = @items.reject { |i| i.name == "g" }
  def first_name = @items[0].name
end

class Tidy
  def initialize = @items = [Item.new("i"), Item.new("j")]
  def tidy
    @items.select! { |i| i.name == "j" }
    nil
  end
  def size = @items.size
  def kinds = @items.map { |i| i.class.name }
end

s = Shelf.new
p s.drop.size
p s.drop.last
p s.first_name

b = Bin.new
p b.keep.empty?
p b.keep.size
p b.first_name

q = Queue2.new
p q.by_name.first
p q.by_name.last
p q.first_name

k = Pick.new
p k.others.first
p k.others.size
p k.first_name

t = Tidy.new
p t.tidy
p t.size
p t.kinds
