# An attribute writer called on a receiver whose class is known only when
# the program runs, storing a value of a subclass into a slot typed as its
# ancestor: `@right.left = node` with @right boxed, @left settled as Node
# (link_right stands above initialize) and node a Column. The switch over
# the receiver's class dropped every class whose slot was not the value's
# own type, so no arm was left and the store raised NoMethodError for a
# writer the receiver has. As a statement and for its value.
class Node
  attr_accessor :left, :right
  def link_right(node)
    @right.left = node
    @right = node
  end
  def initialize
    @left = self
    @right = self
  end
  def name = "node"
end
class Column < Node
  def initialize(name)
    super()
    @name = name
  end
  def name = @name
end
root = Column.new("root")
%w[a b c].each { |n| root.link_right(Column.new(n)) }
plain = Node.new
plain.link_right(Column.new("d"))
p root.left.name, root.right.name, plain.left.name, plain.right.name, plain.right.left.name
x = Column.new("x")
r = root.right
v = (r.right.left = x)
p v.name, r.right.left.name
