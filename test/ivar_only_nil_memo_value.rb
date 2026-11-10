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

# `&&=` on an ivar that holds nothing answers nil without running its value,
# and `||=` on a frozen receiver still refuses the write.
class Guarded
  def and_memo
    @y &&= nil
  end

  def memo
    @z ||= nil
  end
end

p Guarded.new.and_memo
frozen = Guarded.new.freeze
begin
  frozen.memo
  p :written
rescue FrozenError => e
  p e.class
end
