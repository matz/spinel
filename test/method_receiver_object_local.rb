# Method#receiver on a Method looked up on an object held in a local reads
# the object back out of the Method after the local is rebound, at top
# level and in a method on its parameter. The program checks no class of
# its own, so the read must take no class check either (it failed to link
# where it unboxed the receiver through the class chain, which only such
# a check brings in).
class B
  def initialize(n) = (@n = n)
  def n = @n
end
o = B.new(1)
m = o.method(:n)
o = B.new(2)
puts m.receiver.n
puts m.receiver.equal?(o)
def go(o)
  m = o.method(:n)
  o = B.new(4)
  m.receiver.n + o.n
end
puts go(B.new(3))
