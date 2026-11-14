# spinel: int64
# An inject over each.with_index whose block answers a value that cannot go
# back into the numeric seed's slot (a boxed value, a Rational or a Complex)
# accumulates boxed, as the Array inject does. The accumulator stayed an
# Integer slot and the C compiler refused the assignment.

chars = [1, 2, 3]

# a boxed value: the ternary's arms are an Integer and a Rational
puts chars.each.with_index.inject(0) { |sum, (c, k)| sum + c * (k == 1 ? 1r/2 : k) }

# a Rational every turn
puts chars.each.with_index.inject(0) { |sum, (c, k)| sum + c * Rational(1, k + 1) }

# a Complex every turn
puts chars.each.with_index.inject(0) { |sum, (c, k)| sum + Complex(c, k) }

# a pair name instead of a destructuring
puts chars.each.with_index.inject(0) { |sum, pr| sum + pr[0] * (pr[1] == 2 ? 1r/3 : pr[1]) }

# a Float seed, and an Integer seed with an Integer body, keep their slots
puts chars.each.with_index.inject(0.5) { |sum, (c, k)| sum + c * k }
puts chars.each.with_index.inject(10) { |sum, (c, k)| sum + c * k }

# the same fold over an Array of pairs
puts chars.each_with_index.to_a.inject(0) { |sum, (c, k)| sum + c * (k == 1 ? 1r/2 : k) }

# a Bignum block value widens the accumulator to a Bignum, seed and all
puts chars.each.with_index.inject(0) { |sum, (c, k)| sum + c * 2**70 }
puts chars.each.with_index.inject(2**65) { |sum, (c, k)| sum + c * 2**70 }
