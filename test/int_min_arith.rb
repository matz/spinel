# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Arithmetic with a -2**63 operand whose result stays in range in every mode
# (no overflow, so no mode raises): the operand is a number, so no operation
# reports nil.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p m + 1
p m + n
p m - 0
p m - n
p m * 1
p m * (n + 1)
p 1 + m
p m / 1
p m / 2
p m / -2
p m % 7
p m % -7
p m.divmod(2)
p m.remainder(7)
p m.fdiv(2)
p m.succ
p m & 1
p m & m
p m | 0
p m | 1
p m ^ 0
p m ^ m
p m >> 1
p m >> 63
p m >> 64
p m << 0
p m.to_s(2)
p m.to_s(16)
p m.to_s(36)
p "%d" % m
p "%x" % m
p "%020d" % m
p m.bit_length
p m[63]
p m[0]
p m.to_f
p m.to_i
p m.to_r
p m.hash == m.hash
p m.pow(1)
p m.ceil(-3)
p m.truncate
p m.coerce(1)
p m.integer?
