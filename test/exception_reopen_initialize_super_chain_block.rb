# The initialize a reopening above defines may keep a block parameter, which is a
# separate C parameter of it: the super that reaches it passes the block (or none)
# there. The call had too few arguments and the C compiler refused it.
class NameError
  def initialize(msg = nil, &blk)
    @blk = blk
    super("N: #{msg}")
  end
  def blk? = !@blk.nil?
end
class NoMethodError
  def initialize(m = nil) = super("M: " + m.to_s)
end
e = NoMethodError.new("x")
p e.message, e.blk?

# a bare super forwards the caller's block too (a reopening that has a block
# parameter reaching one that does not)
class KeyError
  def initialize(m = nil, &b)
    super
  end
end
class IndexError
  def initialize(m = nil) = super("I:" + m.to_s)
end
p KeyError.new("k").message
p KeyError.new("k") { 1 }.message
