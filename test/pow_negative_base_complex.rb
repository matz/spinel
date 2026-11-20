# spinel: int64
# A boxed negative real raised to a fractional power is a Complex, as
# CRuby's rb_dbl_complex_new_polar_pi makes it: Spinel returned NaN. The
# cases here are a half turn (an exact imaginary part) or NaN, which CRuby
# computes the same with or without libm's __cospi.

def box(x) = [x, :pad][0]

# an Integer, Float, Bignum or Rational base, with a Float exponent
p box(-4) ** box(0.5), box(-8) ** box(-0.5), box(-1) ** box(0.5), box(-4) ** box(2.5)
p box(-2.25) ** box(0.5), box(-4.0) ** box(-1.5), box(-1.5) ** box(2.5)
p box(-(2**70)) ** box(0.5), box(-(2**70)) ** box(2.0)
p box(Rational(-1, 4)) ** box(0.5), box(Rational(-9, 4)) ** box(-0.5)

# a Rational exponent with denominator 2
p box(-4) ** box(Rational(1, 2)), box(-4.0) ** box(Rational(-3, 2)), box(Rational(-1, 4)) ** box(Rational(1, 2))
# a Rational exponent with Bignum parts
p box(-4.0) ** box(Rational(2**70 + 1, 2**71)), box(-4.0) ** box(Rational(-(2**71) - 3, 2**72))

# a NaN exponent; an integral or infinite exponent stays a Float
p box(-8) ** box(Float::NAN), box(-8) ** box(2.0), box(-8.0) ** box(3.0), box(-8) ** box(Float::INFINITY)

# a positive base and a negative zero base stay Floats
p box(9) ** box(0.5), box(2.25) ** box(-0.5), box(-0.0) ** box(0.5)

# the cmath gem's cbrt shape: a method parameter that may be negative
def half_power(z) = z ** 0.5
p half_power(-16), half_power(16), half_power(-6.25)

# a typed Rational ** Rational returns the Complex too
r = Rational(-1, 4)
p r ** Rational(1, 2), r ** Rational(-3, 2)
