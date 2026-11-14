# A boxed Integer, Float or Rational against a Complex compares as two
# Complexes, in both orders; a typed Rational too
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
