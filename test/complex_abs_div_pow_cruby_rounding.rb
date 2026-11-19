# spinel: int64
# Complex#abs, Complex#/ and Complex#** with a real or Complex exponent as
# CRuby computes them
# CRuby uses Math.hypot for abs, Smith's method for a division with a Float
# part, and the polar form of abs ** e and arg * e for a real exponent.
# Spinel used sqrt(re*re + im*im), the conjugate formula and exp(w * log z),
# which differ from CRuby in the last digits.

# abs
p Complex(1.5, -2.5).abs, Complex(6.06093398766667, 7.920372032964854).abs
p Complex(0, 4).abs, Complex(3.0, 0).abs, Complex(-2.5, 0.0).abs

# very large and very small parts, where sqrt(re*re + im*im) and the
# conjugate formula overflow or underflow
p Complex(1e200, 1e200).abs, Complex(1e-200, 1e-200).abs
p Complex(1e300, 1e300) / Complex(1e300, 1e300), Complex(1e-300, 1e-300) / Complex(1e-300, 2e-300)

# an Integer divisor keeps the ratio of its parts exact, as CRuby's Rational
# does, also for Integer parts past 2**53 in the products
p Complex(-7505999378950810.0, 9007199254740972) / Complex(6, 5)
p Complex(4503599627370497.0, 9007199254740972) / Complex(7, 3)
p Complex(1.5, 4611686018427387904) / Complex(3, 4503599627370497)
p Complex(0.25, 9007199254740991) / Complex(9007199254740989, 9007199254740991)

# division with a Float part
p Complex(1.5, 2.5) / Complex(0.5, 1.5), Complex(-0.3, 0.7) / Complex(2.5, 7.25)
p Complex(1.5, 2) / Complex(1, 3), Complex(2, 0.5) / Complex(-4, 3)
p Complex(1, 2) / Complex(0.1, 0.2), 2.5 / Complex(1.5, -2.5)

# the sign of a zero part, as CRuby's f_add and f_sub keep an operand for an
# Integer 0 and give an Integer or Rational zero no sign
p Complex(2.5, 0) / Complex(0, -0.75), Complex(0, -0.0) / Complex(-2, 0)
p Complex(0, 0) / Complex(-0.0, 1), Complex(0, 0) / Complex(1, -0.75)

# fdiv by a Complex: Smith's method in Floats, also for Integer parts
p Complex(0.1, 7).fdiv(Complex(3, 7)), Complex(7, 0.1).fdiv(Complex(3, 7))
p Complex(0.1, 0.3).fdiv(Complex(3, 7)), Complex(3, 4).fdiv(Complex(1, 2))
p Complex(0, 0).fdiv(Complex(0, -2)), Complex(0, 0).fdiv(Complex(-0.0, 1))

# a real exponent
p Complex(3, 4) ** 0.5, Complex(1.5, -2.5) ** (1.0 / 3), Complex(-0.3, 0.7) ** -0.75
p Complex(3, 4) ** Rational(1, 3), Complex(0, 4) ** 0.5, Complex(-4, 0) ** 0.5
p Complex(0.0, 0.0) ** 0.5, Complex(0, 0) ** -0.5

# a Complex exponent, with a Float or an Integer zero imaginary part
p Complex(1.5, -2.5) ** Complex(0.5, 1), Complex(3, 4) ** Complex(2, 0.0)
p Complex(3, 4) ** Complex(0, 1)

# a zero Complex exponent is the Integer 1, also with Float zero parts
p Complex(0, 0) ** Complex(0, 0.0), Complex(0, 0) ** Complex(0.0, 0.0)
p Complex(1, 1) ** Complex(0.0, 0.0), Complex(1.5, 2) ** Complex(0, 0.0), Complex(0, 0) ** 0.0

# a boxed Complex
def pick(f) = f ? Complex(1.5, -2.5) : 1
z = pick(true)
p z.abs, z / Complex(0.5, 1.5), z ** 0.5, z ** Complex(0.5, 1)
