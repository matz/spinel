# `x OP= rhs` on a boxed local that may hold a V whose `+` takes a rest
# parameter: the rest holds an Array of the arguments, so the op-write
# leaves it to the call's own binding rather than typing it as the
# right-hand side (which made the C pass an Array where the binder had
# typed an Integer).
class V
  attr_reader :x
  def initialize(x) = @x = x
  def +(*a) = V.new(@x + a.sum)
end
p (V.new(1) + 2).x
s = [V.new(10), 3][ARGV.size]
s += 5
p s.x
t = [3, V.new(10)][ARGV.size]
t += 4
p t
