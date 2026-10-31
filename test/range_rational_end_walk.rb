# spinel: int64
# A Range walked from an Integer begin up to a Rational end yields the
# same Integers as the end truncated: (1..5r/2) walks 1, 2. Spinel keeps
# these walks (a Rational bound is refused elsewhere: the Range holds it
# truncated).
p((1..5r / 2).to_a)
p((1..3r).sum)
(1..7r / 2).each { |i| print i }
puts
a = [3r, 5r / 2]
p((1..a[0]).to_a)
p((1..a[1]).sum)
p((1..a[1]).map { |i| i * 2 })
x = a[1] * 2
p((0..x).select(&:even?))
b = [1, nil][0]
p((b..5r / 2).to_a)
p((-1..(-9223372036854775807r / 2)).to_a)
