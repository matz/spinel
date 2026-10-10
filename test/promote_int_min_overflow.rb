# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# --int-overflow=promote: an operation on -2**63 whose result leaves int64
# promotes to a Bignum as CRuby does, and the operand itself is a number, not
# the nil of a nullable slot. (promote-only, like the other promote_* tests:
# the default mode raises RangeError here, correctly.)
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p m
p m.nil?
p m.abs
p -m
p m - 1
p m - (n + 1)
p m * 2
p m * (n + 2)
p m + m
p m.pred
p (m - 1) + 1
p (m - 1 + 1) == m
p (m.abs - 1) == 9223372036854775807
p [m, m].sum
p m ** 2
p 1 << 63
p (n + 1) << 63
p (m - 1).class
p [m - 1, m].min
p [m - 1, m].min.nil?
