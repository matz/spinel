# `x OP= rhs` on a boxed local that may hold an A, where A#+
# answers another class (a B): the operator is still called on what the
# slot held, so A#+ takes the right-hand side. The op-write node's class
# facts include the slot's classes as well as the operator's result.
# (opassign_ivar_poly_slot_result_class.rb is the same for an ivar.)
class B
  attr_reader :v
  def initialize(v) = @v = v
end
class A
  attr_reader :v
  def initialize(v) = @v = v
  def +(o) = B.new(o.is_a?(A) ? @v + o.v : @v + o)
end

p (A.new(1) + 5).v
s = [A.new(1), 2.5][ARGV.size]
s += A.new(2)
p s.v

t = [2.5, A.new(10)][ARGV.size]
t += 1
p t
