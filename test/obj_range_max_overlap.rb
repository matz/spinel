# A Range of objects: an exclusive max walks succ, overlap? compares the ends
class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.is_a?(V) ? n <=> o.n : nil; end
  def succ; V.new(n + 1); end
  def inspect; "V#{n}"; end
end
class W
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.is_a?(W) ? n <=> o.n : nil; end
  def inspect; "W#{n}"; end
end
def v(n) = V.new(n)

p (v(1)...v(4)).max, (v(1)..v(4)).max, (v(3)...v(3)).max, (v(5)...v(2)).max
p (v(1)..v(3)).overlap?(v(3)..v(5)), (v(1)...v(3)).overlap?(v(3)..v(5))
p (v(1)..v(3)).overlap?(v(4)..v(5)), (v(1)..).overlap?(v(9)..v(9)), (..v(2)).overlap?(..v(7))
p (v(3)..v(1)).overlap?(v(1)..v(3)), (v(2)..v(2)).overlap?(v(2)...v(2)), (v(2)..v(4)).overlap?(v(2)..v(3))
p (v(1)..v(3)).overlap?(1..3), (v(1)..v(3)).overlap?(nil..nil)
begin
  (v(1)..v(3)).overlap?(v(2))
rescue TypeError => e
  p e.message
end
begin
  (W.new(1)...W.new(3)).max
rescue TypeError => e
  p e.message
end
begin
  (...v(3)).max
rescue RangeError => e
  p e.message
end

def boxed(r, o) = [r.max, r.overlap?(o)]
p boxed(v(1)...v(4), v(3)..v(3)), boxed(1...4, 3..3), boxed(v(1)...v(2), v(2)..v(3))
