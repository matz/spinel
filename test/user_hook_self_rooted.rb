# spinel: gc-stress
# The runtime reaches a user object's #to_a and #coerce, and a Struct's
# members, through hooks the program defines, from helpers that hold the
# object only in their argument: a NodeSet a call has just answered, read as
# an Array, a V a call has just answered, on the right of a Numeric's
# operator, and a Struct a class read out of an Array has just made. The method roots its
# parameters but not self, so under SPINEL_GC_STRESS=2 its first allocation
# freed the object and it read a freed field.
class NodeSet
  include Enumerable
  def initialize(list = []) = @list = list
  def each(&block) = @list.each(&block)
  def [](i, len = nil)
    if len
      NodeSet.new(@list[i, len])
    else
      @list[i]
    end
  end
  def slice(i, len = nil) = self[i, len]
end
ns = NodeSet.new([1, 2, 3])
p ns.slice(0)
p ns.slice(1, 2).to_a, ns.slice(0, 2).map { |x| x * 2 }
p [ns.slice(0, 3), 1][0].to_a
class V
  attr_reader :x
  def initialize(x) = (@x = x)
  def coerce(o) = [V.new(o), self]
  def /(o) = V.new([@x, o.x, "#{@x}/#{o.x}"])
  def +(o) = V.new([@x, o.x])
end
def mk(i) = V.new("v#{i}")
a = [1, 2.5][0]
p (a / mk(1)).x, (a + mk(2)).x
r = []
3.times { |i| r << ([3, "s"][0] / mk(i)).x }
p r
P2 = Struct.new(:a, :b)
u = [P2, 0][0]
p u[3, 4].to_a, u[5, 6].to_h
