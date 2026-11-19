# spinel: int64
# real, imaginary and conjugate on a boxed Rational with Bignum parts
# A boxed Rational answers respond_to? true for these, and the small
# Rational ran them, but one whose numerator is a Bignum raised
# NoMethodError.
def pick(f) = f ? Rational(2**70, 3) : 1
z = pick(true)
s = [1, Rational(3, 4)][1]

p z.respond_to?(:real), z.respond_to?(:imaginary), z.respond_to?(:imag)
p z.respond_to?(:conjugate), z.respond_to?(:conj)
p z.real, z.imaginary, z.imag, z.conjugate, z.conj
p z.real == z, z.conj == z, z.real?

# the small Rational and an Integer, as before
p s.real, s.imaginary, s.imag, s.conjugate, s.conj
i = pick(false)
p i.real, i.imaginary, i.conjugate

# guarded as a caller would
p(z.respond_to?(:real) ? z.real : :skipped)
