# Math.sqrt of a Complex made only by a negative real ** a fraction returns a
# Complex, as CRuby's rb_math_sqrt does. Spinel compiled the Float Math.sqrt,
# which raised TypeError, unless the program also named Complex.

xs = [-4, 2.0]
p Math.sqrt(xs[0] ** 0.5)
p Math.sqrt(xs[1] ** 0.5)
p Math.sqrt(xs[0] ** Rational(1, 2))
p Math.sqrt(xs[0] ** 2)
