# A `>` or `<=>` of an unrelated class does not change how a sort, min or max
# block reads a Float or Rational answer by its sign.
class Vec; def >(o) = true; def <(o) = false; end
class Num2; include Comparable; def <=>(o) = 0; end
module Ord; def <=>(o) = 0; end
class Num3; include Ord; end
class Numeric; def <=>(o) = super; end
p [3, 1, 2].max { |x, y| Rational(y - x, 7) }
p [3, 1, 2].min { |x, y| (y - x) * 0.5 }
p [3, 1, 2].sort { |x, y| (y - x) * 0.5 }
p [0.3, 0.1, 0.2].sort { |x, y| (y - x) }
p %w[bb a ccc].max { |x, y| (x.size - y.size) / 2.0 }
p [3, 1, 2].sort { |x, y| next 0.5 if x == y; (y - x) * 0.5 }
p [1,2,3].sort { |x, y| 2**70 * (y <=> x) }
p [3, 1, 2].sort { |x, y| Rational(y - x, 7) }
p Vec.new > 1, Num2.new < Num2.new
p Num3.new <=> Num3.new
p [3, 1, 2].max { |x, y| Rational(y - x, 7) }
