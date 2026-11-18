# `@x OP= rhs` on a boxed ivar that may hold an A, where A#+
# answers another class (a B): the operator is still called on what the
# ivar held, so A#+ takes the right-hand side. The op-write node's class
# facts include the ivar's classes as well as the operator's result.
# (opassign_poly_slot_result_class.rb is the same for a local.)
class B
  attr_reader :v
  def initialize(v) = @v = v
end
class A
  attr_reader :v
  def initialize(v) = @v = v
  def +(o) = B.new(o.is_a?(A) ? @v + o.v : @v + o)
end

class Holder
  def initialize(x) = @acc = x
  def bump(o)
    @acc += o
    @acc.v
  end
end

p (A.new(1) + 5).v
p Holder.new([A.new(3), 0.5][ARGV.size]).bump(A.new(4))
