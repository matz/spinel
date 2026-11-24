# spinel: int64
# The numerator and denominator of a big Rational are Bignums, and its
# rationalize with an epsilon is the simplest Rational within the epsilon.
# numerator raised RangeError for a Bignum outside --int-overflow=promote,
# and rationalize with an epsilon raised RangeError in each mode.
z = Rational(2**70, 3)
p z.numerator
p z.numerator + 1
p Rational(1, 2**70).denominator
p z.rationalize(Rational(1, 100))
p z.rationalize(0.01)
p z.rationalize(Rational(2**69, 1))
p((-z).rationalize(Rational(1, 2)))
p Rational(-(2**70), 3).rationalize(Rational(1, 10))
p Rational(2**70, 3).rationalize(Rational(1, 10))
p Rational(1, 2**70).rationalize(Rational(1, 2**142))
p Rational(2**70 + 1, 2**70).rationalize(Rational(1, 100))
p Rational(355 * 2**64 + 1, 113 * 2**64).rationalize(Rational(1, 10**6))
p Rational(-(2**64 + 1), 2**63).rationalize(0.01)

boxed = [z, "x"][0]
p boxed.numerator
p boxed.rationalize(Rational(1, 100))
