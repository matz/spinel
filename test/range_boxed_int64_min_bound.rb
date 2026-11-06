# spinel: int64 -- -2**63 is past a 32-bit sp_int
# -2**63 fits a 64-bit Range bound, though its box can stay a Bignum.
lo = -(2**63)
lo = "a" if ARGV.size > 5
p((lo..0) === -5)
p((lo..0).first(1))
