# spinel: int64
# A boxed Integer, Float or Rational against a Complex compares as two
# Complexes, in both orders; a typed Rational too; Struct#eql? stays strict
nums = [1, 1.0, Rational(1, 1), 2, 0.5, Rational(1, 2), 2**70, "x", nil, :s]
cs = [Complex(1, 0), Complex(1, 1), Complex(0.5, 0), Complex(2, 0), Complex(1.0, 0)]
nums.each { |n| p cs.map { |c| [n == c, n != c, c == n] } }
p [Complex(1, 0)].include?(1)
p [1, "a"].index(Complex(1, 0))
p({1 => :a}.key?(Complex(1, 0)))
x = [Complex(1, 0), "s"].first
y = [1, "s"].first
p x == y, y == x, x != y
case y
when Complex(1, 0) then p :hit
else p :miss
end
p Rational(1, 2) == Complex(0.5, 0), Rational(1, 2) != Complex(0.5, 0), Rational(1, 2) === Complex(0.5, 0)
p Rational(1, 3) == Complex(1, 3)
p 1 == Complex(1, 0), 1.0 == Complex(1, 0)
p [1, 1.0, Rational(1)].count(Complex(1, 0))
p [Complex(1, 0), Complex(1, 1)].uniq.size
P = Struct.new(:a, :b); D = Data.define(:x)
p P.new(Complex(1, 0), 2).eql?(P.new(1, 2))
p P.new(1, 2).eql?(P.new(Complex(1, 0), 2))
p D.new(x: Complex(1, 0)).eql?(D.new(x: 1))
p P.new(1, 2).eql?(P.new(1.0, 2)), P.new(1, 2) == P.new(1.0, 2)
p P.new([1], 2).eql?(P.new([1.0], 2)), P.new("a", 2).eql?(P.new("a", 2))
p P.new(1, 2).eql?(P.new(1, 2)), D.new(x: 1).eql?(D.new(x: 1))
c = Complex(2**53, 0)
a = [2**53 + 1, "s"].first
p a == c, a != c, c == a
p [2**53, "s"].first == c
p [Complex(1, 2), 1].first == Complex(1, 2)
p [Float::NAN, 1].first == Complex(Float::NAN, 0)
q = [Rational(2**53 + 1, 1), "s"].first
p q == Complex(2**53, 0), Rational(2**53 + 1, 1) == Complex(2**53, 0)
p Rational(2**53 + 1, 1) != Complex(2**53, 0), Rational(2**53 + 1, 1) == Complex(2.0**53, 0)
