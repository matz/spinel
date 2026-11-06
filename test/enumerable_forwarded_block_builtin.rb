# A method forwarding its block to a builtin (`@nodes.map(&block)`) gives its
# caller's block what the builtin yields, when a method of the program shares
# the builtin's name. The method called in the block is called nowhere else, so
# the block's parameter is all that keeps it.
class Ns
  def initialize(prefix) = @prefix = prefix
  def prefix = @prefix
end
class Node
  def initialize(prefix) = @prefix = prefix
  def ns = Ns.new(@prefix)
  def pair = Ns.new(@prefix + "!")
end
class Nodes
  def initialize(nodes) = @nodes = nodes
  def map(&block)
    @nodes.map(&block)
  end
  def each_with_index(&block)
    @nodes.each_with_index(&block)
  end
  def walk(&block)
    Walker.new(@nodes).each(&block)
  end
end
class Walker
  def initialize(nodes) = @nodes = nodes
  def each(&block)
    @nodes.each(&block)
  end
end
class Table
  def initialize(h) = @h = h
  def each(&block)
    @h.each(&block)
  end
end
class One
  def initialize(n) = @n = n
  def each
    yield @n
  end
end
class Either
  def initialize(x) = @x = x
  def each(&block)
    @x.each(&block)
  end
end
list = Nodes.new([Node.new("a"), Node.new("b")])
p list.map { |n| n.ns.prefix }
p list.map { |n| n.ns }.last.prefix
list.each_with_index { |n, i| puts n.pair.prefix + i.to_s }
list.walk { |n| puts n.ns.prefix }
Table.new({ "k" => Node.new("c") }).each { |k, n| puts k + n.ns.prefix }
Either.new(One.new(Node.new("d"))).each { |n| puts n.ns.prefix }
Either.new([Node.new("e")]).each { |n| puts n.ns.prefix }
