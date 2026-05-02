# Float#to_s / p / puts must produce CRuby-byte-identical output:
# the shortest decimal that round-trips, fixed-point inside CRuby's
# [-4, 15] decimal-exponent window, scientific (`d.ddde+NN`) outside.
# Pre-fix, sp_float_to_s used `%g` (default precision 6) and emitted
# scientific for any value whose decpt exceeded 6 — so e.g. a
# Time.now-sized float printed as `1.7777e+09`.

# Whole values keep the `.0` suffix.
puts 1.0
puts 100.0
puts(-3.25)
puts 1234567890.0
puts 1234567890.5

# Round-trip-tricky decimals.
puts 0.1
puts 0.3
puts 0.30000000000000004

# Negative-exponent boundary: -3 stays fixed, -4 flips to scientific.
puts 0.0001
puts 0.00001

# Positive-exponent boundary: decpt == 15 stays fixed, 16+ flips.
puts 1.5e14
puts 1.0e15
puts 9.99e15
puts 1.0e16
puts 1.0e100

# Signed zero, ±Infinity, NaN all match CRuby's spelling.
puts(-0.0)
puts Float::INFINITY
puts(-Float::INFINITY)
puts Float::NAN

# Same values via `p` (Float#inspect is aliased to to_s).
p 1.0
p 1234567890.5
p 1.0e16
p(-0.0)
