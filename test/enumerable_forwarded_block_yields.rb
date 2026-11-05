# A method forwarding its block to a call on a receiver (`left.each(&block)`,
# a tree walking its children) yields what the callee yields: Enumerable over
# such an each sees every class of node, and a call in the block dispatches on
# all of them, the ones that inherit the method included.
module Nodes
  class Node
    include Enumerable
    attr_reader :left
    def initialize(left) = @left = left
    def each(&block)
      block.call(self)
      left.each(&block) if left.is_a?(Node)
    end
    def symbol? = false
    def group? = false
  end
  class Terminal < Node; end
  class Symbol < Terminal
    def symbol? = true
  end
  class Binary < Node
    attr_reader :right
    def initialize(left, right) = (super(left); @right = right)
    def each(&block)
      block.call(self)
      left.each(&block) if left.is_a?(Node)
      right.each(&block) if right.is_a?(Node)
    end
  end
  class Cat < Binary; end
  class Group < Node
    def group? = true
  end
end
class Mapping
  def initialize = @spec = nil
  def spec=(s)
    @spec = s
  end
  def names = @spec.find_all(&:symbol?).map { |n| n.left }
  def groups = @spec.find_all(&:group?).flat_map { |g| g.find_all(&:symbol?) }.size
end
m = Mapping.new
m.spec = Nodes::Cat.new(Nodes::Terminal.new("/"), Nodes::Cat.new(Nodes::Symbol.new(":id"), Nodes::Group.new(Nodes::Symbol.new(":fmt"))))
p m.names, m.groups
