# Math.sqrt of a Complex made only by a negative Integer .pow a fraction
# returns a Complex, as CRuby's rb_math_sqrt does. Spinel compiled the Float
# Math.sqrt, which raised TypeError. The file has no ** or Complex.

xs = [-4, 16, 2.0]
p Math.sqrt(xs[0].pow(0.5))
p Math.sqrt(xs[1].pow(0.5))
