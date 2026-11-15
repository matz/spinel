# sort_by, sort_by!, and sort and sort! with a comparator, over an Array of
# one class. Each kept the array a boxed poly array, and every read of an
# element went through the boxed dispatch: the narrowing modeled only the
# blockless sort and sort!, and the emitters took no sp_PtrArray. Now each
# is a filter of the narrowed array (a new array of the same elements, or
# the array itself), and infer-test checks that @items is an sp_PtrArray.
# A sort_by block that writes its parameter leaves it boxed: the element is
# boxed into it (it was assigned as the raw pointer, which C refused).
class Ver
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(other) = n <=> other.n
end

class Item
  attr_reader :id, :name, :ver
  def initialize(id, name)
    @id = id
    @name = name
    @ver = Ver.new(-id)
  end
end

class Store
  attr_reader :items
  def initialize = @items = []
  def add(id, name) = @items << Item.new(id, name)
  def by_name = @items.sort_by { |i| i.name }
  def by_key = @items.sort_by { |i| [i.name, -i.id] }
  def by_ver = @items.sort_by { |i| i.ver }
  def desc = @items.sort { |a, b| b.id <=> a.id }
  def names = @items.sort { |a, b| a.name <=> b.name }.map { |i| i.name }
  def nil_key = @items.sort_by { |i| i.id > 2 ? nil : i.id }
  def nil_cmp = @items.sort { |a, b| a.id > 2 ? nil : a.id <=> b.id }
  def param_written = @items.sort_by { |i| i = nil if i.id > 3; i ? i.id : 0 }

  def order_by_id
    @items.sort_by! { |i| i.id }
    nil
  end

  def order_by_name
    @items.sort! { |a, b| [a.name, a.id] <=> [b.name, b.id] }
    nil
  end

  def local_desc
    list = @items
    list.sort_by { |i| -i.id }
  end
end

s = Store.new
s.add(3, "c")
s.add(1, "b")
s.add(2, "a")
s.add(4, "b")
p s.by_name.map(&:id)
p s.by_key.map(&:id)
p s.by_ver.map(&:id)
p s.desc.map(&:id)
p s.names
p s.local_desc.map(&:id)
p s.param_written.map(&:id)
p s.items.map(&:id)

copy = s.by_name
copy << Item.new(9, "z")
p [copy.size, s.items.size]

begin
  s.nil_key
rescue ArgumentError => e
  puts "nil key: #{e.class}"
end
begin
  s.nil_cmp
rescue ArgumentError => e
  puts "nil comparison: #{e.class}"
end

s.order_by_id
p s.items.map(&:id)
s.order_by_name
p s.items.map(&:id)
