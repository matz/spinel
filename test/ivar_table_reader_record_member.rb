# A record class's member name does not box another class's ivar table
# A Symbol spelling a reader's name keeps the table the reader hands out
# boxed, as send(:items) or method(:items) may reach the reader. The members
# of Struct.new(:items) and Data.define(:tags), the keyword naming a member
# in a constructor call (Label.new(tags: ...)) and the names a Struct's
# each_pair yields are the record class's own accessors and reach no other
# class's: Shelf's @items and @tags stay tables of Book. Crate's tables,
# which send and a computed name reach, stay boxed.

Book = Struct.new(:title, :pages)
Order = Struct.new(:items, :total)
Label = Data.define(:tags)
Bin = Struct.new(:tags, keyword_init: true)

class Shelf
  attr_reader :items, :tags

  def initialize(n)
    @items = Array.new(n) { |i| Book.new("b#{i}", 100 + i) }
    @tags = Array.new(n) { |i| Book.new("t#{i}", i) }
  end

  def pages = @items.sum(&:pages) + @tags.sum(&:pages)
end

class Crate
  attr_reader :boxes, :lids

  def initialize(n)
    @boxes = Array.new(n) { |i| Book.new("c#{i}", i) }
    @lids = Array.new(n) { |i| Book.new("d#{i}", i * 2) }
  end
end

shelf = Shelf.new(3)
p shelf.pages
p shelf.items.map(&:title), shelf.tags.last.pages

order = Order.new([1, 2], 3)
order.each_pair { |k, v| p [k, v] }
p Label.new(tags: %w[a b]).tags
p Bin.new(tags: [4]).to_h

crate = Crate.new(2)
p crate.send(:boxes).map(&:pages)
name = "li" + "ds"
p crate.public_send(name.to_sym).map(&:title)
