# An element read of one Array of a class's objects stored straight into
# another (`node.links << nodes[j]`) is evidence of the class the first one
# holds, as the same read through a local is (`nb = nodes[j]`). The pushed
# read alone reads boxed while its source is decided again each round, so
# the target took no class and stayed boxed with its walks. A source that
# stays boxed gives no evidence: its element pushed elsewhere keeps the
# target boxed. infer-test greps the C: Node's and Ring's lists are an
# sp_PtrArray *, Bin's, filled from a mixed Array, stays an sp_PolyArray *.
# An Array.new block's `next v` is one of its elements as its tail is.
class Node
  attr_reader :id, :links
  def initialize(id) = (@id = id; @links = [])
end

nodes = Array.new(5) { |i| Node.new(i) }
nodes.each_with_index do |node, i|
  node.links << nodes[(i + 1) % 5]
  node.links.push(nodes.at((i + 2) % 5))
end
nodes[0].links[0] = nodes[4]
total = 0
nodes.each { |n| n.links.each { |l| total += l.id * 10 + n.id } }
p total
p nodes[0].links.map(&:id)

# the source an ivar, the target another class's ivar
class Ring
  attr_reader :members
  def initialize = @members = []
end

class Hub
  def initialize(k)
    @nodes = Array.new(k) { |i| Node.new(i + 10) }
    @rings = Array.new(2) { Ring.new }
    @nodes.each_with_index do |_, i|
      r = @rings[i % 2]
      r.members << @nodes[i]
    end
  end

  def sizes = @rings.map { |r| r.members.sum { |m| m.id } }
end
p Hub.new(5).sizes

# a source that holds two classes stays boxed, and so does the target
class Leaf
  attr_reader :id
  def initialize(id) = @id = id
end

class Bin
  attr_reader :items
  def initialize = @items = []
end

mixed = [Node.new(20), Leaf.new(21), Node.new(22)]
bin = Bin.new
3.times { |i| bin.items << mixed[i] }
p bin.items.map { |x| x.id + 1 }
p bin.items.map(&:class)

# an Array.new block's `next v` is an element too: a `next` of another class
# keeps the Array boxed, whether the tail is an element read or a new object,
# and so does a boxed `next` (an element of a mixed Array) or a boxed tail
# beside a typed `next`; one of the same class narrows it as the tail alone
# would
class Other
  def id = -1
end
src = [Node.new(30), Node.new(31)]
ids = []
mixed_read = Array.new(4) { |i| next Other.new if i.odd?; src[0] }
mixed_read.each { |x| ids << x.id }
mixed_new = Array.new(4) { |i| next Other.new if i.odd?; Node.new(i) }
mixed_new.each { |x| ids << x.id }
same = Array.new(4) { |i| next src[1] if i.odd?; Node.new(i) }
same.each { |x| ids << x.id }
holes = Array.new(3) { |i| next if i == 1; Node.new(i) }
holes.each { |x| ids << (x ? x.id : 0) }
boxed_next = Array.new(3) { |i| next mixed[1] if i == 0; src[0] }
boxed_next.each { |x| ids << x.id }
boxed_tail = Array.new(3) { |i| next src[0] if i == 0; mixed[1] }
boxed_tail.each { |x| ids << x.id }
p ids

