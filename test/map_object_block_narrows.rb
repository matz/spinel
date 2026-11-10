# spinel: gc-stress
# A `map` whose block answers one class builds an Array of that class, as
# `Array.new(n) { Obj.new }` does: an sp_PtrArray, its elements read unboxed.
# It stayed a boxed poly Array, so every reader dispatched on the class and
# boxed the fields it read. What still keeps it boxed: a `next` or a `break`
# in the block, a tail of another class, a boxed tail, a Hash receiver, and a
# reader the narrowing does not model.

class Cell
  attr_accessor :v, :links
  def initialize(v) = (@v = v; @links = [])
end

class Other
  def v = -1.0
end

# a Range, an Array, a block parameter, an ivar layer read by a method
def range_cells(n) = (1..n).map { Cell.new(1.5) }
def array_cells(xs) = xs.map { |x| Cell.new(x * 0.5) }

class Layer
  def initialize(n) = (@cells = (0...n).map { |i| Cell.new(i + 0.25) })
  def total
    s = 0.0
    @cells.each { |c| s += c.v }
    s
  end
  def link
    @cells.each { |c| @cells.each { |d| c.links << d } }
    @cells.sum { |c| c.links.size }
  end
end

a = range_cells(4)
b = array_cells([1.0, 2.0, 3.0])
p a.size, a[2].v, b.map(&:v), b.sum(0.0) { |c| c.v }
l = Layer.new(5)
p l.total, l.link

# many layers, garbage between them
acc = 0.0
40.times do |k|
  m = (0...30).map { |i| Cell.new(i * 0.5 + k) }
  junk = Array.new(50) { |i| "j#{i}" }
  acc += m.sum(0.0) { |c| c.v } + junk.size
end
p acc

# stays boxed: a next, a break, two classes, a boxed tail, a Hash receiver
def nexts(n) = (0...n).map { |i| next Cell.new(0.0) if i.odd?; Cell.new(i * 1.0) }
def breaks(n) = (0...n).map { |i| break [] if i > 5; Cell.new(1.0) }
def mixed(n) = (0...n).map { |i| i.odd? ? Cell.new(2.0) : Other.new }
def boxed(xs) = xs.map { |x| x }
def from_hash(h) = h.map { |k, v| Cell.new(v) }
p nexts(4).map(&:v), breaks(3).size, breaks(9), mixed(3).map(&:v)
p boxed([Cell.new(3.0), Cell.new(4.0)]).map(&:v), boxed([Cell.new(3.0), Other.new]).map(&:v)
p from_hash({ a: 1.0, b: 2.5 }).map(&:v)

# readers of a mapped Array of objects, as of one Array.new built
class P
  include Comparable
  attr_reader :n
  def initialize(n) = (@n = n)
  def <=>(o) = n <=> o.n
  def to_s = "P#{@n}"
  def inspect = "#<P #{@n}>"
end
ps = (0...3).map { |i| P.new(3 - i) }
p ps.min, ps.max, ps.sort.map(&:n), ps.minmax.map(&:n)
puts ps.join(","), "#{ps}", ps.inspect
q = (0...3).map { |i| P.new(i) }
q << P.new(7)
q[1] = nil
p q.compact.map(&:n), q.size, q.count(&:nil?)
