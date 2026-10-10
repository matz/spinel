# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 reached by arithmetic that does not overflow (no mode raises) is a
# number in a plain Integer local, not the nil of a nullable slot: every read
# that used to compare the word against INT64_MIN sees the value.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p m
puts m
puts m.to_s
puts m.inspect
puts "#{m}"
puts "[" + m.to_s + "]"
print m, "\n"
p m.nil?
p(m ? "truthy" : "falsy")
puts "yes" if m
p !m
p m.zero?
p m.negative?
p m.even?
p m.class
p m == nil
p nil == m
p m.eql?(nil)
p m.equal?(nil)
p(m || 0)
p(m && 1)
p m&.succ == nil ? "nil" : "value"
p [m].inspect
p m.frozen?
