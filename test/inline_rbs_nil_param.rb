# An inline RBS signature may type a parameter as plain `nil`: the slot only
# ever holds nil. No C slot kind carries just nil, so the parameter binds
# boxed, as a nil-only local does, and the method still compiles.
class Node
  attr_reader :ctx #: nil

  #: (String, nil) -> void
  def initialize(name, ctx)
    @name = name
    @ctx = ctx
  end

  #: (?ctx: nil) -> Node
  def self.build(ctx: nil)
    new("built", ctx)
  end

  def describe
    "#{@name}: #{@ctx.inspect}"
  end
end

n = Node.new("a", nil)
p n.ctx
puts n.describe
puts Node.build.describe
puts Node.build(ctx: nil).describe

module Counter
  #: (nil) -> Integer
  def self.count(x)
    x.nil? ? 1 : 0
  end
end
p Counter.count(nil)
