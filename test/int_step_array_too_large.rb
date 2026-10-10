# spinel: not-cruby -- CRuby answers a lazy sequence here (its size, its first elements), which spinel does not have.
# spinel: int64
# The array a blockless Integer#step builds refuses a span past 2**30 elements
# with RangeError, as Range#step's does (and the Float form's), rather than
# pushing until memory runs out: the sp_int form, and the boxed form a
# Rational receiver takes. A small span still builds its array.
n = ARGV.size
a = -5000000000000000000 - n
b = 5000000000000000000 + n

def t
  p yield
rescue RangeError => e
  p e.message
end

t { a.step(b, 3).to_a.size }
t { a.step(b, 3).size }
t { a.step(b, 3).first(2) }
t { b.step(a, -3).to_a.size }
t { Rational(n).step(1 << 40, 1).to_a.size }
t { Rational(n).step(-(1 << 40), -1).size }
t { (1 + n).step(10, 3).to_a }
t { 10.step(1 + n, -4).to_a }
t { (1 + n).step(0, 1).to_a }
t { Rational(n).step(2, 1).to_a }
