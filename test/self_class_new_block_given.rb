# self.class.new(&b) hands the block to an initialize that asks
# block_given?, as Bd.new(&b) does (Rack::Builder#generate_map's shape)
class Bd
  def initialize(&block)
    @got = block_given? ? block.call : :none
  end
  attr_reader :got
  def again(b) = self.class.new(&b).got
  def again_const(b) = Bd.new(&b).got
  def bare = self.class.new.got
end
class Guarded
  def initialize(&block)
    @got = block ? block.call : :none
  end
  attr_reader :got
  def again(b) = self.class.new(&b).got
end
class Yields
  def initialize
    @got = block_given? ? yield(:x) : :none
  end
  attr_reader :got
  def again(b) = self.class.new(&b).got
  def bare = self.class.new.got
end
p Bd.new { :a }.got
p Bd.new.again(proc { 1 }), Bd.new.again_const(proc { 2 }), Bd.new.bare
p Guarded.new.again(proc { 3 })
p Yields.new.again(proc { |v| [v, 4] }), Yields.new.bare
