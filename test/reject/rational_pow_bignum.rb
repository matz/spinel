# Rational ** an Integer past 64 bits answers a Rational in CRuby; computed
# in floats it would answer a Float, so it is refused.
def big
  2**64
end
p Rational(1, 1) ** big
