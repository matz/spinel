# A named `&blk` forwarded into an anonymous `&` forwarder, which hands it
# to a method keeping the block: the forwarder is spliced into the named
# one, whose block is a proc, and the anonymous forward found no literal
# block and passed nil.
class Keeper
  def install(&h) = @h = h
  def fire(v) = @h.call(v)
end
class Front
  def initialize = @k = Keeper.new
  def install(&) = @k.install(&)
  def fire(v) = @k.fire(v)
end
class Outer
  def initialize = @f = Front.new
  def install(&blk) = @f.install(&blk)
  def fire(v) = @f.fire(v)
end
o = Outer.new
o.install { |v| p v * 2 }
o.fire(5)
