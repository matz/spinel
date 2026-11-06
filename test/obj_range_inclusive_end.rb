# A walk over a Range of objects stops at a member equal to the inclusive end
# without calling its succ (range_each_func): that succ may raise.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ
    raise "succ past end" if n >= 3
    V.new(n + 1)
  end
end

p (V.new(1)..V.new(3)).to_a.map(&:n)
r = []
(V.new(1)..V.new(3)).each { |v| r << v.n }
p r
p (V.new(1)..V.new(3)).map(&:n)
p (V.new(1)..V.new(3)).first(5).map(&:n)
p (V.new(1)...V.new(3)).to_a.map(&:n)
b = [V.new(1)..V.new(3), 1][0]
p b.map { |v| v.n }
p b.first(4).map(&:n)

