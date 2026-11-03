# A reader called on a boxed value -- here one taken out of a mixed pair,
# `[node, -1]` -- reads the list of the one class of the program that the
# value can be and that has the reader: a call of the list's reader on such
# a value kept every class's list of that name boxed. The class facts bound
# the value; where they leave another class with the reader (Box beside
# Crate), a builtin with a method of the name (Pile's `first`), or the list
# read that way takes another kind of element (Bin), it stays boxed.
# infer-test greps the C: Node's @links is an sp_PtrArray *, and Box's,
# Crate's, Pile's and Bin's stay sp_PolyArray *.
class Node
  attr_reader :id, :links
  def initialize(id) = (@id = id; @links = [])
end

class Box
  attr_reader :id, :items
  def initialize(id) = (@id = id; @items = [])
end

class Crate
  attr_reader :id, :items
  def initialize(id) = (@id = id; @items = [])
end

class Pile
  attr_reader :id, :first
  def initialize(id) = (@id = id; @first = [])
end

class Bin
  attr_reader :id, :slots
  def initialize(id) = (@id = id; @slots = [])
end

nodes = Array.new(4) { |i| Node.new(i) }
nodes.each_with_index { |node, i| nb = nodes[(i + 1) % 4]; node.links << nb }
pairs = [[nodes[0], -1]]
n, _ = pairs[0]
p n.links.size
p n.links.map(&:id)
total = 0
nodes[0].links.each { |l| total += l.id }
p total

boxes = [Box.new(1), Box.new(2)]
crates = [Crate.new(3), Crate.new(4)]
boxes[0].items << boxes[1]
crates[0].items << crates[1]
either = [[boxes[0], 0], [crates[0], 0]]
either.each { |pair| x, _ = pair; p x.items.size }
p boxes[0].items.sum(&:id) + crates[0].items.sum(&:id)

piles = [Pile.new(5), Pile.new(6)]
piles[0].first << piles[1]
mixed = [[piles[0], 0], [[7, 8], 0]]
mixed.each { |pair| y, _ = pair; f = y.first; p(f.is_a?(Array) ? f.size : f) }
p piles[0].first.sum(&:id)

bins = [Bin.new(9), Bin.new(10)]
bins[0].slots << bins[1]
held = [[bins[0], -1]]
z, _ = held[0]
s = z.slots
s << 11
p bins[0].slots.size
p bins[0].slots.map { |e| e.is_a?(Bin) ? e.id : e }
