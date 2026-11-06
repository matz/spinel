# The pair #coerce makes answers <=> with a Float (or nil). The number beside
# the object asks #coerce once, and reads a Float answer by its sign: in a
# sort block, and in between? and clamp on a boxed number.
class Wq
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.n == 99 ? nil : (n - o.n) * 0.5; end
end
class Nq
  attr_reader :n
  def initialize(n); @n = n; end
  def coerce(x); puts "coerce #{x}"; [Wq.new(x), Wq.new(n)]; end
end
begin
  p [5, Nq.new(99)].sort { |x, y| x <=> y }
rescue ArgumentError => e
  p e.message
end
p [5, Nq.new(3)].sort { |x, y| x <=> y }.map { |v| v.is_a?(Integer) ? v : v.n }
xs = [5, 2, "s"]
n3 = Nq.new(3)
p xs[0].between?(1, n3), xs[1].between?(1, n3)
p xs[0].clamp(1, n3).class, xs[1].clamp(1, n3)
