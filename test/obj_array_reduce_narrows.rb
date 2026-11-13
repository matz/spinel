# spinel: gc-stress
# A reduce or an inject over an Array of one class kept it a boxed
# sp_PolyArray: narrow_object_arrays left every fold out, its emitter walking
# only numeric rows. The fold now binds each element as its sp_X *, as the
# each-family walks do, and the Array narrows. Seeds of every kind, no seed,
# an empty Array, next and break, a nil element (NoMethodError, as CRuby), an
# accumulator that is an element, and many folds allocating as they go.

class Cell
  attr_accessor :v, :name
  def initialize(v, name = "c") = (@v = v; @name = name)
  def <=>(o) = v <=> o.v
end

class Bin
  def initialize(n) = (@cells = Array.new(n) { |i| Cell.new(i * 1.5, "c#{i}") })

  def total = @cells.reduce(0.0) { |acc, c| acc + c.v }
  def int_total = @cells.inject(0) { |acc, c| acc + c.v.to_i }
  def promoted = @cells.inject(0) { |acc, c| acc + c.v }
  def biggest = @cells.reduce { |a, c| c.v > a.v ? c : a }
  def from_first = @cells.reduce(@cells[0]) { |a, c| c.v < a.v ? c : a }
  def names = @cells.reduce([]) { |a, c| a << c.name }
  def by_name = @cells.inject({}) { |h, c| h[c.name] = c.v; h }
  def joined = @cells.reduce("") { |s, c| s + c.name }
  def built = @cells.reduce(Cell.new(0.0)) { |a, c| Cell.new(a.v + c.v) }
  def skip_big = @cells.reduce(0.0) { |a, c| next a if c.v > 3.0; a + c.v }
  def stop_at(x) = @cells.reduce(0.0) { |a, c| break a if c.v > x; a + c.v }
  # the block answers what the accumulator's slot is not: an element into a
  # number seed, and with no seed a String, an Integer or nil beside the
  # element the accumulator starts as
  def last_cell = @cells.reduce(0) { |acc, c| c }
  def last_name = @cells.reduce { |acc, c| c.name }
  def seven = @cells.reduce { |a, c| 7 }
  def even_or_nil = @cells.reduce { |a, c| c.v > 3.0 ? nil : c }
  def add_nil = (@cells << nil; nil)
  def add_cell = (@cells << Cell.new(9.0); nil)
  # the accumulator takes an element, nil where the Array holds one
  def chained = @cells.reduce(Cell.new(-1.0)) { |a, c| a.v > 100 ? a : c }
end

b = Bin.new(5)
p b.total, b.int_total, b.promoted, b.biggest.name, b.from_first.name
p b.names, b.by_name, b.joined, b.built.v, b.skip_big, b.stop_at(2.0)

def show(x) = x.is_a?(Cell) ? "Cell #{x.name}" : x
[5, 1, 0].each do |n|
  m = Bin.new(n)
  p [m.last_cell.class, show(m.last_name), show(m.seven), m.even_or_nil&.name]
end

e = Bin.new(0)
p e.total, e.biggest, e.names, e.built.v

p b.chained.v
b.add_nil
begin
  p b.total
rescue NoMethodError => ex
  p ex.class
end
b.add_cell
begin
  p b.chained
rescue NoMethodError => ex
  p ex.class
end

acc = 0.0
40.times do |k|
  m = Bin.new(3 + k % 5)
  junk = Array.new(30) { |i| "g#{i}" }
  acc += m.total + m.built.v + m.biggest.v + junk.size
end
p acc
