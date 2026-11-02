# A call on `t.first` or `t.last` of a table waits for the table's class
# A call on an element of a table this pass is narrowing (`@scans[0].items`)
# is decided once the table's class is known, as the element is then that
# class's object. `@scans.first` and `@scans.last` read an element (or nil)
# the same way, but were taken as calls on a value of any class: `items` on
# it could reach every class's reader of the name, so Reader's own @items,
# which no such call reaches, stayed boxed. It now narrows; Scan's @items,
# which the reader reads through the element, stays boxed, as such a read
# is one the pass does not vet.

Item = Struct.new(:id, :w)

class Scan
  attr_reader :items

  def initialize(n) = @items = Array.new(n) { |i| Item.new(i, i) }
end

class Reader
  attr_reader :items

  def initialize(n, k)
    @items = Array.new(n) { |i| Item.new(i, i * 2) }
    @scans = Array.new(k) { |i| Scan.new(i + 2) }
  end

  def first_items = @scans.first&.items || []
  def last_size = @scans.last.items.size
end

r = Reader.new(4, 3)
p r.items.map(&:w).max, r.first_items.size, r.last_size
p Reader.new(1, 0).first_items
