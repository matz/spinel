# spinel: int64 -- the bounds are the ends of a 64-bit Integer
# A Range or a bit slice whose bound is an end of a 64-bit Integer: the span
# and the field width are counted without overflowing (`last - first` at
# 2**63-1 and -2**63, `last - 1` at -2**63, a shift by 64 or more).
n = ARGV.size
hi = 9223372036854775807 - n
lo = -9223372036854775808 + n
p (hi - 2..hi).to_a
p (hi - 2...hi).to_a
p (lo..lo + 2).to_a
p (lo...lo).to_a
p (lo..lo + 2).count
p (hi - 2..hi).count
p 5[0..2], 5[2..1], 5[1...1], 5[2..2], 5[2...2]
p (-1)[60..62], 0b1011[1..62]
p 7[64..70], (-8)[64..66]
