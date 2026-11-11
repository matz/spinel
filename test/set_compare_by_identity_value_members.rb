# spinel: int64 -- values past 2^31; not run on a 32-bit target
# spinel: not-cruby
# An identity Set (or Hash) given two equal Floats that are no flonum, or
# two equal Integers past the fixnum range that fit 64 bits, made apart,
# keeps them as one member: Spinel holds those by value and takes equal
# ones as one object (docs/limitations.md), where CRuby keeps two. Two
# Rationals or two Bignums made apart stay two, as in CRuby, and one
# object added twice is one member.
require "set"
n = ARGV.size + 1
pairs = [[1e300 * n * 10, 1e300 * n * 10], [-0.0 * n, -0.0 * n], [2**62 * n, 2**62 * n],
         [1r / (3 * n), 1r / (3 * n)], [2**64 * n, 2**64 * n], [7 * n, 7 * n]]
pairs.each do |a, b|
  s = Set.new.compare_by_identity
  s << a
  s << b
  s << a
  p [a.class, s.size]
end
