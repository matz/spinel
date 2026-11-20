# Math.sqrt of a Complex made only by a negative real **= a fraction returns
# a Complex, as CRuby's rb_math_sqrt does. Spinel compiled the Float
# Math.sqrt, which raised TypeError. The file has no other ** or Complex.

xs = [-4, 2.0]
y = xs[0]
y **= 0.5
p Math.sqrt(y)
xs[0] **= 0.5
p Math.sqrt(xs[0])
@z = xs[1]
@z **= 0.5
p Math.sqrt(@z)
