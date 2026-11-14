# spinel: int64
# Complex * a real number multiplies each part, so each part keeps its
# own kind: Complex(1.0, 2) * 3 is (3.0+6i), not (3.0+6.0i).
n = Float::NAN
inf = Float::INFINITY
z = Complex(1.0, 2)
p z * 3, z * 3.0
p Complex(1, 2) * 3, Complex(1, 2.5) * 2, Complex(1, 2) * 0
f = ARGV.size + 4611686018427387904
p 3 * z, 3.0 * Complex(1, 2), 0 * z, (f / 2) * z
p f * z, (-f - 1) * z, (-f) * Complex(1.0, 0), (f - 1) * Complex(1.0, 0)
p Complex(n, 1) * 3.0, Complex(n, 1) * 3, 3 * Complex(n, 1)
p 3.0 * Complex(1, n), 3.0 * Complex(inf, 1), 3 * Complex(inf, 1)
w = Complex(1.0, 2)
w *= 3
p w
w *= 2.0
p w
w *= [3, 2.5].first
p w
w *= [Complex(1, 1), 1].first
p w
p (Complex(-1, 2) * 0) * 1.0, Complex(-1, 2) * 0
p 3.0 * Complex(-0.0, -1.0), 3.0 * Complex(1.0, -0.0), -2.0 * Complex(1.0, 0.0)
p 0.0 * Complex(inf, 1), 0.0 * Complex(1, inf), -0.0 * Complex(2, -3), 0.0 * Complex(-0.0, 2.0)
p [3.0, 1].first * Complex(-0.0, -1.0), [0.0, 1].first * Complex(inf, 1)
b = [Complex(1.0, 2), 1].first
p b * 3, 3 * b, b * 3.0, [3, 1].first * b, [f, 1].first * b, [2**70, 1].first * b
p b * [3, 1].first, [f - 1, 1].first * Complex(1.0, 0), [f, 1].first * Complex(1.0, 0)
p (z * 3).imaginary.class, (3 * z).imaginary.class, (b * 3).imaginary.class
p (z * 3).eql?(Complex(3.0, 6)), [z * 3, 1].first.eql?(Complex(3.0, 6))
