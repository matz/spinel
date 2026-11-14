# spinel: int64
# An Integer part of a Complex stays an Integer past 2**63: it reads,
# prints and compares as an Integer (a Bignum), not as the Float that holds it.
m = ARGV.size + 2**62
z = Complex(m, 1) * 2
p z.real
p z.real.class
p z
p z.to_s
p z.rectangular
w = Complex(m, 1) * 4
p w.real, w.real.class, w.imaginary
p [w, 1].first.real
p w.to_s
p Complex(m, 0) * 4 == [2**64, 1].first
p [Complex(m, 0) * 4, 1].first == [2**64, 1].first
v = Complex(1, m) * 4
p v, v.imaginary, v.imaginary.class
n = Complex(-m, 0) * 4
p n, n.real, n.real.class
p Complex(-m, 0) * 2, (Complex(-m, 0) * 2).real.class
p Complex(2**32, 0), Complex(2**32, 0).real, Complex(2**32, 0) == [2**32, 1].first
p Complex(2**31, 3).real.class
p (Complex(m, 0) * 4).real == 2**64
