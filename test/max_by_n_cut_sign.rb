# spinel: int64
# max_by(n) and min_by(n) keep more than 16: the buffer is cut, and the cut
# reads a key's `<=>` answer by its sign, so a key answering the most
# negative Integer (whose negation overflows) orders as CRuby's does
wide = Array.new(80) { |i| (i * 37) % 80 }
class Far
  attr_reader :v
  def initialize(v) = @v = v
  def <=>(o) = v == o.v ? 0 : (v < o.v ? -(2**62) * 2 : 2**62)
end
p wide.max_by(17) { |x| Far.new(x) }, wide.min_by(17) { |x| Far.new(x) }
