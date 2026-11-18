# spinel: int64
# Bigint results of every size: a value of a few words, one just past
# 128 bits, and the results that cross between them both ways, with zero
# and negative ones. Many are kept in an Array across later allocations.
MASK64 = 0xFFFF_FFFF_FFFF_FFFF

def show(label, v)
  puts "#{label}: #{v}"
end

big1 = 18446744073709551616                            # 2**64
big2 = 340282366920938463463374607431768211456         # 2**128
big3 = big2 * big2 / 18446744073709551616 / 4096 + 12345  # 2**180 + 12345

show "mask", -1 & MASK64
show "mask of a negative", -123_456_789 & MASK64
show "2**64 - 1 + 1", MASK64 + 1
show "just past 128 bits", big2 * 2 + 1
show "back below 128 bits", (big2 * 2 + 1) >> 70
show "back to a word", big3 >> 190
show "sub to zero", big1 - big1
show "and to zero", big1 & (big1 - 1)
show "negative small", -(big1 * 64) + 5
show "negative large", -(big3 * big3)
show "xor", big1 ^ (big1 + 0xFF)
show "or", big1 | 1
show "not", ~big1
show "shr floor negative", (-(big1 * big1 / 268435456) - 1) >> 3
show "shl", big1 << 66
show "mul", (big1 / 2) * (big1 / 2)
show "div", big3 / big1
show "mod", big3 % 1_000_000_007
show "remainder", (-big3).remainder(97)
show "divmod", big2.divmod(big1 + 1).inspect
show "pow", (big1 + 1)**3
show "pow mod", (big1 + 3).pow(5, 1_000_000_007)
show "pow mod big", (big1 + 3).pow(5, big1 + 13)
show "gcd", (big1 * 3).gcd(big1 * 9)
show "lcm", (big1 * 4).lcm(big1 * 6)
show "isqrt", Integer.sqrt(big2 * 9)
show "isqrt large", Integer.sqrt(big3 * big3)
show "digits", (big1 + 7).digits(1000).inspect
show "to_s 16", (big2 - 1).to_s(16)
show "round", (big3 + 499).round(-3)

kept = []
x = MASK64
2000.times do |i|
  x = (x * 6364136223846793005 + i) & MASK64
  kept << x if i % 7 == 0
  kept << x * x * x if i % 101 == 0
end
show "kept", kept.size
show "kept sum", kept.sum
show "kept max", kept.max
show "marshal", Marshal.load(Marshal.dump([big1 + 1, -(big3), 0, MASK64])).inspect
show "u64 unpack", [0, 4294967296, 9223372036854775808, MASK64].map { |v| [v].pack("Q<").unpack1("Q<") }.inspect
