# spinel: int64
# spinel: not-cruby -- a typed negative base raises Math::DomainError, where CRuby returns a Complex
# A typed negative Rational, Integer or Bignum base raised to a fractional
# power raises Math::DomainError, as a typed Float base does, where Spinel
# returned NaN for a Rational base or exponent (docs/limitations.md).
# Promote boxes these values, and a boxed base returns the Complex, so the
# test stays out of that lane.
r = Rational(-1, 4)
i = -8
f = -8.0
p(r ** 0.5) rescue puts "raised1"
p(Rational(-9, 4) ** -0.5) rescue puts "raised2"
p(i ** Rational(1, 2)) rescue puts "raised3"
p(f ** Rational(1, 2)) rescue puts "raised4"
p(i.pow(0.5)) rescue puts "raised5"
big = -(2**70)
p(big ** 0.5) rescue puts "raised6"

# a positive base or an integral exponent stays a Float
p(Rational(1, 4) ** 0.5, 8 ** Rational(1, 3), 4.0 ** Rational(1, 2), r ** 2.0)
