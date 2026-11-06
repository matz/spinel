# A Rational past 64 bits is Comparable and Numeric, as a small one is.
r = Rational(2**70, 3)
w = [r, 1r, 3]
p w.map { |x| [x.is_a?(Comparable), x.is_a?(Numeric), Comparable === x, (case x when Comparable then 1 else 0 end)] }
p r.is_a?(Comparable)
p [Rational(2**70, 3), Rational(1, 2**70)].map { |x| x.kind_of?(Comparable) }
