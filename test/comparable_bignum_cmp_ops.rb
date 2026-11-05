# A <=> whose result is a Bignum (here (a - b) * 2**70) gives its sign to
# every Comparable operator: ==, !=, <, <=, >, >=, between? and clamp.
class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); (n - o.n) * (2**70); end
end
# An Integer for some pairs, a Bignum for others.
class M
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n > 5 ? n - o.n : (n - o.n) * (2**70); end
end
class W < V; end
Pt = Struct.new(:n) do
  include Comparable
  def <=>(o) = (n - o.n) * (2**64)
end
a, b = V.new(2), V.new(3)
p a == V.new(2), a != V.new(2), a == b, a != b
p a < b, a > b, a <= V.new(2), a >= b
p a.between?(a, b), a.clamp(b, b).n
p M.new(2) < M.new(3), M.new(7) > M.new(6), M.new(2) == M.new(2)
p W.new(1) < W.new(2), W.new(2) == W.new(2)
p Pt.new(1) < Pt.new(2), Pt.new(2) == Pt.new(2), Pt.new(3) >= Pt.new(4)
xs = [a, b, W.new(9)]
p xs.count { |x| x > a }
def lt(x, y) = x < y
p lt(a, b), lt(b, a)
p [b, a].max.n, [b, a].sort.map(&:n)
