# Complex#== and #!= with a boxed operand that is no number: unequal, as
# in CRuby, and no conversion of the operand to Float
c = Complex(1, 2)
m = ["x", nil, :s, [1], 1, 2.0, Rational(3), Complex(1, 2)]
m.each { |v| p(c == v) }
m.each { |v| p(c != v) }
p(Complex(1, 0) == m[4])
p(Complex(2, 0) == m[5])
p(Complex(3, 0) == m[6])
p(Complex(1, 0) != m[4])
p(Complex(0, 0) == m[0])
p(Complex(0, 0) != m[1])
