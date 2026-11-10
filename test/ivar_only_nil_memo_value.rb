# An ivar only nil is written to has no slot; its `||=` as a value answers
# the nil it writes, so a method ending in it answers nil, not nothing.
class Node
  def initialize(parent)
    @parent = parent
  end

  def root
    @root ||= (@parent ? @parent.root : nil)
  end

  def cached
    @cached ||= nil
  end

  def relative_root
    @relative_root ||= begin
      other = Object.new
      (other.relative_root if other.is_a?(Node))
    end
  end

  def relative_root?
    !relative_root.nil?
  end
end

leaf = Node.new(Node.new(nil))
p leaf.root
p leaf.root.nil?
p leaf.cached.nil?
p leaf.relative_root?
