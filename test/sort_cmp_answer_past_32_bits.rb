# spinel: int64
# A <=> answer past 32 bits orders by its sign: sort did not drop the high bits.
class V
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = (n - o.n) * 4294967296
end
a = [V.new(3), V.new(1), V.new(2)]
p a.sort.map(&:n)
b = a.dup; b.sort!; p b.map(&:n)
p a.minmax.map(&:n)
p a.max(2).map(&:n), a.min(2).map(&:n)
p a.sort.reverse.map(&:n)
p [V.new(5), 3.0, V.new(1)].map { |x| x.is_a?(V) ? x : V.new(x.to_i) }.sort.map(&:n)
h = { V.new(2) => 1, V.new(1) => 2 }
p h.sort.map { |k, v| k.n }
p h.min_by { |k, v| k }[0].n
p a.sort_by { |x| x }.map(&:n)
class W
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = (n - o.n) * -4294967297
end
p [W.new(1), W.new(3), W.new(2)].sort.map(&:n)
