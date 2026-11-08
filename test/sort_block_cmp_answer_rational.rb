# spinel: int64
# A sort, min, max or minmax block whose <=> answers a Rational orders the
# pair by the Rational's sign, and a Comparable operator reads a Rational
# <=> answer by its sign too.
class VR
  include Comparable
  attr_reader :n
  def initialize(n) = (@n = n)
  def <=>(o) = Rational(@n - o.n, 3)
  def inspect = "VR#{@n}"
end
BIG = Rational(10**20, 3)
class VB
  include Comparable
  attr_reader :n
  def initialize(n) = (@n = n)
  def <=>(o) = BIG * (@n - o.n)
  def inspect = "VB#{@n}"
end
a = [VR.new(3), VR.new(1), VR.new(2)]
p a.sort { |x, y| x <=> y }
p a.sort { |x, y| y <=> x }
p a.min { |x, y| x <=> y }
p a.max { |x, y| x <=> y }
p a.minmax { |x, y| x <=> y }
p a.max(2) { |x, y| x <=> y }
p a.min(2) { |x, y| x <=> y }
b = [VB.new(3), VB.new(1), VB.new(2)]
p b.sort { |x, y| x <=> y }
p b.min { |x, y| x <=> y }
p b.max(2) { |x, y| x <=> y }
x, y, z = VR.new(1), VR.new(2), VR.new(1)
p x < y, x <= y, x > y, x >= y, x == z, x == y
p y.between?(x, VR.new(3)), y.clamp(x, x)
p VB.new(1) < VB.new(2), VB.new(2).between?(VB.new(1), VB.new(2))
p a.sort, b.max
# a Rational <=> whose operand is a String
class VS
  include Comparable
  def initialize(n) = (@n = n)
  def <=>(o) = Rational(@n - o.size, 3)
end
p VS.new(3) < "ab", VS.new(1) < "ab", VS.new(2) == "ab", VS.new(3).between?("a", "abcd")
p VS.new(2).clamp("abc", "abcd").size
