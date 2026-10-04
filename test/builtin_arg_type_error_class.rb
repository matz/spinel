# A builtin method handed an argument of a kind it cannot take raises the
# error CRuby raises: scan's TypeError for a pattern that is no pattern,
# transpose's TypeError for elements that are no Arrays, rationalize's
# NoMethodError for an epsilon with no #abs, and Rational#fdiv's
# RangeError for a quotient with an imaginary part. Each raised
# NoMethodError, or a conversion's TypeError.

p(("hello".scan(1) rescue [$!.class, $!.message]))
p(("hello".scan(:l) rescue [$!.class, $!.message]))
p(([1, 2].transpose rescue [$!.class, $!.message]))
p((["a"].transpose rescue [$!.class, $!.message]))
p [].transpose, [[1, 2], [3, 4]].transpose
p((2.5.rationalize("x") rescue [$!.class, $!.message]))
p 2.5.rationalize(Rational(1, 100))
p((Rational(1, 2).fdiv(Complex(2, 2)) rescue $!.class))
