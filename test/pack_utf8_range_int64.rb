# spinel: int64
# spinel: share
# spinel: gc-minor
# Array#pack("U") out-of-range codepoints above 2^31-1 (including Bignum and
# out-of-range Float values that must not wrap modulo 2^64).

def check(label)
  res = yield
  puts "#{label}: ok #{res.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

check("int 0x7FFFFFFF") { [0x7FFFFFFF].pack("U").bytes }
check("int 0x80000000") { [0x80000000].pack("U") }
check("int 0x100000000") { [0x100000000].pack("U") }
check("int 2**63-1") { [9223372036854775807].pack("U") }
check("int -2**63") { [-9223372036854775808].pack("U") }

check("flt 2147483647.9") { [2147483647.9].pack("U").bytes }
check("flt 2147483648.0") { [2147483648.0].pack("U") }
check("flt -9223372036854775808.0") { [-9223372036854775808.0].pack("U") }
check("flt 9223372036854775808.0") { [9223372036854775808.0].pack("U") }
check("flt 2^64") { [18446744073709551616.0].pack("U") }
check("flt 1e20") { [1.0e20].pack("U") }
check("flt -1e20") { [-1.0e20].pack("U") }
check("flt NaN") { [Float::NAN].pack("U") }
check("flt Inf") { [Float::INFINITY].pack("U") }
check("flt -Inf") { [-Float::INFINITY].pack("U") }

# Poly array with Bignums (must not wrap mod 2^64 into valid ASCII).
big_hi = 2**63
big_wrap = 2**64 + 65
big_neg = -(2**64) + 65
check("bignum 2^63") { [big_hi].pack("U") }
check("bignum 2^64+65") { [big_wrap].pack("U") }
check("poly 2^63") { ["a", big_hi].pack("aU") }
check("poly 2^64+65") { ["a", big_wrap].pack("aU") }
check("poly -2^64+65") { ["a", big_neg].pack("aU") }
check("buf 2^64+65") { ["a", big_wrap].pack("aU", buffer: +"pre") }
check("poly flt 2^64") { ["a", 18446744073709551616.0].pack("aU") }
check("buf flt 2^64") { ["a", 18446744073709551616.0].pack("aU", buffer: +"pre") }
