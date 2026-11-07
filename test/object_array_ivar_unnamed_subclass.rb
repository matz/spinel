# An ivar narrowed to an object array is narrowed in every subclass's copy
# of the slot, a subclass nothing names included: its struct is still laid
# out under the base's, and the layout check compared a poly array field
# against the base's object array (`require "pp"` with no pp call: PP under
# PrettyPrint).
class Group
  attr_reader :depth
  def initialize(depth) = @depth = depth
end

class Base
  def initialize
    @stack = [Group.new(0)]
  end

  def top = @stack.last
end

class Sub < Base
end

puts Base.new.top.depth
