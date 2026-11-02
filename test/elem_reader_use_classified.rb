# A reader called on an element of a table being narrowed (`nodes[0].links`)
# is a read of the reader's ivar, and what the call over it does is
# classified as for any read of that ivar: a push, a []= or a walk gives
# evidence, and another class pushed through it, or the read handed to a
# method, keeps the ivar's Array boxed.

class Node
  attr_reader :id, :links
  def initialize(id) = (@id = id; @links = [])
end

class Other
  def id = 99
end

def take(a) = a << :sym

nodes = Array.new(3) { |i| Node.new(i) }
nodes.each { |node| node.links << nodes[0] }
nodes[1].links << Other.new
nodes[2].links[0] = Other.new
p nodes.map { |n| n.links.map(&:id) }

class Cell
  attr_reader :id, :near
  def initialize(id) = (@id = id; @near = [])
end

cells = Array.new(2) { |i| Cell.new(i) }
cells.each { |cell| cell.near << cells[0] }
take(cells[1].near)
p cells[1].near.map { |e| e.is_a?(Cell) ? e.id : e }
