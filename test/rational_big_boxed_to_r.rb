# spinel: int64
# to_r and rationalize on a boxed big Rational return the Rational itself.
# The call was typed as a small Rational, and the big Rational's two Bignum
# pointers were read as its numerator and denominator.
z = Rational(2**70, 3)
p z.to_r, z.rationalize
p z.to_r == z, z.to_r + 1
def conv(x) = x.to_r
p conv(z), conv(2), conv(Rational(5, 2))
small = [Rational(3, 4), "x"][0]
p small.to_r, small.to_r + Rational(1, 4)
p small.rationalize, small.rationalize(Rational(1, 10))
p z&.to_r, small&.rationalize
n = [nil, 1][0]
p n.to_r, n.rationalize, n.to_r + 1
i = [7, "s"][0]
p i.to_r, i.rationalize
f = [0.5, "s"][0]
p f.to_r, f.rationalize, f.rationalize(Rational(1, 10))
half = [Rational(1, 2), "x"][0].to_r
p (1..2).step(half).to_a, (1..2).step([0.5, "x"][0].rationalize).to_a
p 1.step(2, [Rational(1, 2), "x"][0].to_r).to_a
