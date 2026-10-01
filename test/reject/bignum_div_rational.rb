# Integer#div with a Rational divides exactly in CRuby; the Bignum division
# takes Integers only, so it is refused rather than truncating the Rational.
def big
  2**64
end
p big.div(Rational(4, 1))
