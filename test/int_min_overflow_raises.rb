# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# spinel: not-cruby -- fixed-width overflow raises instead of promoting to Bignum.
# An operation on -2**63 whose result leaves int64 raises RangeError in the
# default mode like any other overflow: the operand is a number, so the error
# is the overflow, never a NoMethodError for nil. (The .expected is Spinel's;
# CRuby promotes to a Bignum, pinned by promote_int_min_overflow.rb.)
n = ARGV.size
m = -9223372036854775807 - (n + 1)
def t(label)
  print label, ": "
  p yield
rescue RangeError
  puts "RangeError"
rescue => e
  puts "#{e.class}: #{e.message}"
end
t("abs")   { m.abs }
t("neg")   { -m }
t("sub")   { m - 1 }
t("sub n") { m - (n + 1) }
t("mul")   { m * 2 }
t("mul n") { m * (n + 2) }
t("add")   { m + m }
t("value") { m }
t("value nil?") { m.nil? }
