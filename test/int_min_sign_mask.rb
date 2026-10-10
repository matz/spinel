# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# The sign-bit mask of a 64-bit word, ~0x7fffffffffffffff, is -2**63: a
# literal fold and a run-time complement both give the number.
n = ARGV.size
sign = ~0x7fffffffffffffff
p sign
p sign.nil?
p(sign ? "mask set" : "no mask")
x = 0x7fffffffffffffff - n
rsign = ~x
p rsign
p rsign.nil?
p rsign == sign
p x ^ (-1 - n)
p((-1 - n) & ~x)
p(((-5 - n) & rsign) != 0)
p(((5 + n) & rsign) != 0)
p rsign.to_s(2).size
p rsign.to_s(16)
p rsign.bit_length
p(rsign & 0x7fffffffffffffff)
p(rsign | 0x7fffffffffffffff)
p(rsign >> 63)
p(rsign ^ rsign)
p [rsign].pack("Q<").unpack1("q<")
p (rsign >> 1) & 0x7fffffffffffffff
v = 0x1234 - n
p v | rsign
p (v | rsign) & ~rsign
p (v | rsign).negative?
p (v | rsign).nil?
puts "#{v | rsign}"
