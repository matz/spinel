# An identity Hash keyed by objects of the program's that reach it through
# a reader called on an Array's element, and through a method whose `case`
# ends in a `raise` arm: each key is an object compared by pointer, so the
# Hash is the identity Hash CRuby has.
class Node
  attr_reader :left
  def initialize(left) = @left = left
end
class Lit < Node; end
class Cat < Node; end

def by_reader(xs, a, b)
  table = Hash.new { |h, k| h[k] = [] }.compare_by_identity
  xs.each { |x| table[x.left] += [x] }
  x = xs[0]
  table[x.left] += [:first]
  [table.size, table[a].size, table[b].size, table.compare_by_identity?]
end

def leaf(node)
  case node
  when Cat then leaf(node.left)
  when Lit then node
  else raise ArgumentError, "unknown node"
  end
end

def by_case(nodes, a, b)
  table = {}.compare_by_identity
  nodes.each { |n| table[leaf(n)] = (table[leaf(n)] || 0) + 1 }
  [table.size, table[a], table[b], table.key?(Lit.new(nil))]
end

a = Lit.new(nil)
b = Lit.new(nil)
p by_reader([Cat.new(a), Cat.new(b), Cat.new(a)], a, b)
p by_case([Cat.new(a), a, Cat.new(Cat.new(b)), Cat.new(a)], a, b)
begin
  leaf(Node.new(nil))
rescue ArgumentError => e
  p e.message
end
