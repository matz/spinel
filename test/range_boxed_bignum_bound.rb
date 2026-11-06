# spinel: not-cruby -- CRuby builds the Range; Spinel's Range holds sp_int bounds, so a Bignum one is refused
# A Range bound read out of a box that holds a Bignum: sp_Range has nowhere
# to keep it, as the compile-time refusal says of one known statically.
# Converted, it was cut to its low bits and `(128...m) === 252` answered
# false; it raises NotImplementedError instead. A boxed Integer bound still
# builds the Range.
m = 1 << 64
m = "a" if ARGV.size > 5
begin
  p((128...m) === 252)
rescue NotImplementedError => e
  p e.message
end
begin
  p((m..) === 252)
rescue NotImplementedError => e
  p e.message
end
n = 300
n = "a" if ARGV.size > 5
p((128...n) === 252)
p((128...n).size)
