# A method ending in a walk answers the walked Array only while the walk is
# Array's own. A module that defines `each`, put ahead of a class anywhere in
# the program (prepend, extend), may be ahead of Array's, so the walk's value
# is not known to be its receiver: the list and the method's return stay
# boxed together. With the list narrowed under the boxed return, the C did
# not compile.
module Overrides
  def each
    :overridden
  end
end

class Other
  prepend Overrides
end

class Cell
  attr_reader :x, :neighbors
  attr_accessor :kind
  def initialize(x) = (@x = x; @kind = 0; @neighbors = [])
end

class Maze
  def initialize(n)
    @flat = Array.new(n) { |i| Cell.new(i) }
    @start = @flat[0]
    @flat.each_with_index do |c, i|
      a = @flat[(i + 1) % n]
      c.neighbors << a
    end
  end

  def open(i)
    cell = @flat[i]
    cell.kind = 1
    return if cell.x > 4
    t = 0
    cell.neighbors.each { |n| t += n.x }
  end

  def run
    open(1)
    open(7)
    @start.neighbors.size + @flat.sum { |c| c.kind }
  end
end

p Maze.new(9).run
p Other.new.each
