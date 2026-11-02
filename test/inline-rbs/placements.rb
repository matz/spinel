# Native annotation syntax at locations ruby/rbs does not use for type facts.
# None may change the C or output from compiling with --no-inline-rbs.
#: (untyped) -> untyped
def at_top(x) = x

#: (untyped) -> untyped
def self.on_main(x) = x

class Other; end
class Static
  class << Other
    #: (untyped) -> untyped
    def enclosed(x) = x
  end

  #: (untyped) -> untyped
  def Other.foreign(x) = x

  # @rbs @before_attr: String?
  attr_reader :before_attr

  # @rbs @before_method: String?
  def initialize
    @before_attr = nil
    @before_method = nil
    @block_declared = nil
    @before_constant = nil
  end

  # @rbs @before_constant: String?
  MARKER = 1

  def before_constant = @before_constant
  def before_method = @before_method
  def endless(x) = x #: untyped

  class_eval do
    # @rbs @block_declared: String?

    def block_declared = @block_declared
  end
end

Dynamic = Class.new do
  #: (untyped) -> untyped
  def value(x) = x
end

Pair = Struct.new(:x) do
  #: (untyped) -> untyped
  def value(y) = x + y
end

Point = Data.define(:x) do
  #: (untyped) -> untyped
  def value(y) = x + y
end

Static.class_eval do
  #: (untyped) -> untyped
  def evaluated(x) = x
end

p at_top(1), on_main(2), Other.enclosed(3), Other.foreign(4)
s = Static.new
p s.before_attr, s.before_method, s.endless(5), s.evaluated(6), s.block_declared, s.before_constant
p Dynamic.new.value(7), Pair.new(8).value(1), Point.new(9).value(1)
