# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# --int-overflow=promote: the operations on -2**63 whose exact result leaves
# int64 -- abs, unary minus, a Range sum or count over it, 1 - it -- answer
# CRuby's Bignum. (promote-only, like the other promote_* tests: the default
# mode raises RangeError for each of these, correctly; the in-range reads of
# the same value are pinned by int_min_compare_case, int_min_range_begin,
# int_min_range_iteration and nullable_int_right_operand.)
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p m.abs
p m.abs == m.abs
p m.abs.class
p -m
p (-m).class
r = (m..m + 2)
p r.sum
p r.sum.class
p (m..0).count
p (m..0).size
p (m..m + 2).reduce(:+)
p (m..m + 2).inject { |s, i| s + i }
p 1 - m
p 0 - m
p [m, m].sum
p [m, -1].sum
p m * 2
p m - 1
p m.pred
p m - (n + 1)
p (m - 1).abs
y = [m].find { |e| e < 0 }
p 1 - y
p y.abs
p -y
p m.abs - 1 == 9223372036854775807
p (m.abs - 1).class
