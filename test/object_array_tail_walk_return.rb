# A method ending in a walk over an Array of a class's objects answers
# that Array, and an early bare `return` answers nil. Nothing reads these
# methods' values, so the walks' values are thrown away and the Arrays
# narrow; the methods' returns narrow with them. A list read through a
# value typed only after the last round (`@cell0 = @cells[0]`) narrowed
# late, under a return still the boxed Array of the earlier rounds, and
# the C did not compile. infer-test greps the C: each class's @links is an
# sp_PtrArray *, and so is the return of each method ending in a walk and
# of pick; first_far, whose walk breaks with another value, answers an
# sp_RbVal.
class Cell
  attr_reader :x, :links
  attr_accessor :seen
  def initialize(x) = (@x = x; @seen = false; @links = [])
end

class Spot
  attr_reader :x, :links
  attr_accessor :seen
  def initialize(x) = (@x = x; @seen = false; @links = [])
end

class Tile
  attr_reader :x, :links
  attr_accessor :seen
  def initialize(x) = (@x = x; @seen = false; @links = [])
end

class Knot
  attr_reader :x, :links
  def initialize(x) = (@x = x; @links = [])
end

class Grid
  def initialize(n)
    @cells = Array.new(n) { |i| Cell.new(i) }
    @spots = Array.new(n) { |i| Spot.new(i) }
    @tiles = Array.new(n) { |i| Tile.new(i) }
    @knots = Array.new(n) { |i| Knot.new(i) }
    @cell0 = @cells[0]
    @spot0 = @spots[0]
    @tile0 = @tiles[0]
    @cells.each_with_index do |c, i|
      a = @cells[(i + 1) % n]
      b = @cells[(i + 3) % n]
      c.links << a
      c.links << b
    end
    @spots.each_with_index do |c, i|
      s = @spots[(i + 2) % n]
      c.links << s
    end
    @tiles.each_with_index do |c, i|
      t = @tiles[(i + 1) % n]
      c.links << t
    end
    @knots.each_with_index do |c, i|
      k = @knots[(i + 1) % n]
      c.links << k
    end
  end

  # a bare `return`, then `each`
  def open(i)
    cell = @cells[i]
    cell.seen = true
    return if cell.x > 4
    cell.links.each { |c| open(c.x) unless c.seen }
  end

  # `return nil`, then `each`
  def visit(i)
    spot = @spots[i]
    spot.seen = true
    return nil if spot.x > 5
    spot.links.each { |s| visit(s.x) unless s.seen }
  end

  # `reverse_each` with no early return
  def mark(i)
    tile = @tiles[i]
    tile.seen = true
    tile.links.reverse_each { |t| mark(t.x) unless t.seen }
  end

  # a walk that breaks answers the break's value, not the Array
  def first_far(i)
    return if i > 7
    @knots[i].links.each { |k| break k if k.x > 2 }
  end

  def run
    @cell0.links.each { |c| open(c.x) unless c.seen }
    @spot0.links.each { |s| visit(s.x) unless s.seen }
    @tile0.links.each { |t| mark(t.x) unless t.seen }
    first_far(0)
    [@cells.count(&:seen), @spots.count(&:seen), @tiles.count(&:seen)]
  end
end

# a bare `return` answers nil beside the list a method hands back
class Node
  attr_reader :id
  def initialize(id) = (@id = id)
end

def pick(xs, flag)
  return if flag
  xs
end

p Grid.new(9).run
nodes = Array.new(3) { |i| Node.new(i + 1) }
got = pick(nodes, false)
p got.sum { |n| n.id }
