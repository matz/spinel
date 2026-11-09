# spinel: int64
# A sort, min or max block whose answer is a Float, a Rational or a
# Bignum orders by the answer's sign, as CRuby's rb_cmpint does. An
# answer between -1 and 1 is not a tie.
a = [3, 1, 2]
p a.max { |x, y| Rational(y - x, 7) }
p a.max { |x, y| (y - x) / 7.0 }
p a.min { |x, y| Rational(y - x, 7) }
p a.min { |x, y| (y - x) / 7.0 }
p a.sort { |x, y| Rational(y - x, 7) }
p a.sort { |x, y| (y - x) / 7.0 }
p a.max(2) { |x, y| (x - y) / 9.0 }
p a.min(2) { |x, y| Rational(x - y, 9) }
p a.minmax { |x, y| (y - x) / 7.0 }
b = a.dup
b.sort! { |x, y| Rational(y - x, 7) }
p b

# a Bignum answer
p a.max { |x, y| (y - x) * 2**70 }
p a.sort { |x, y| (y - x) * 2**70 }

# Float elements, String elements, Hash pairs, a Range
f = [1.5, 0.5, 2.5]
p f.max { |x, y| (y - x) / 10 }
p f.sort { |x, y| (x - y) / 10 }
s = %w[bb a ccc]
p s.max { |x, y| Rational(x.size - y.size, 5) }
p s.sort { |x, y| (y.size - x.size) * 0.1 }
h = { a: 3, b: 1 }
p h.sort { |x, y| Rational(x[1] - y[1], 4) }
p h.max { |x, y| (x[1] - y[1]) * 0.25 }
p (1..5).max { |x, y| Rational(y - x, 10) }
p (1..5).sort { |x, y| Rational(y - x, 10) }
p (1..4).max { |x, y| (y - x) * 0.5 }
p (1..4).min { |x, y| (y - x) / 2.0 }
p (1..4).minmax { |x, y| (y - x) * 0.5 }

# the program's objects
Pt = Struct.new(:f)
o = [Pt.new(1.5), Pt.new(0.2), Pt.new(0.9)]
p o.sort { |m, n| m.f - n.f }.map(&:f)
p o.max { |m, n| m.f - n.f }.f
p o.min { |m, n| (m.f - n.f).to_r }.f
class W
  attr_reader :v
  def initialize(v) = @v = v
end
w = [W.new(3), W.new(1), W.new(2)]
p w.sort { |m, n| Rational(m.v - n.v, 10) }.map(&:v)
p w.max { |m, n| (m.v - n.v) / 10.0 }.v

# a method's answer, and a `next` of another kind than the tail
def half_cmp(x, y) = (x <=> y) * 0.5
p a.sort { |x, y| half_cmp(x, y) }
p a.max { |x, y| half_cmp(x, y) }
c = [3, 1, 2, 5]
p c.sort { |x, y| next Rational(y - x, 9) if x == 3; (y - x) * 0.5 }
p c.max { |x, y| next -0.5 if x == 5; Rational(y - x, 9) }

# 0.0 and -0.0 tie, so equal elements keep their order
p a.sort { |x, y| -0.0 }
