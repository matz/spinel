# BigDecimal#round and the seven ROUND_* modes (#4881). Every mode, on
# positive and negative values, exact halves (x.5, x.25, x.125), and the
# three n regimes (positive, zero, negative).
#
# A negative value that rounds to zero would be CRuby's signed -0.0, which
# sign*mantissa cannot represent; #round raises there rather than answer
# +0.0, so no such case is asked here.
require "bigdecimal"

MODES = {
  ROUND_UP: BigDecimal::ROUND_UP,
  ROUND_DOWN: BigDecimal::ROUND_DOWN,
  ROUND_HALF_UP: BigDecimal::ROUND_HALF_UP,
  ROUND_HALF_DOWN: BigDecimal::ROUND_HALF_DOWN,
  ROUND_HALF_EVEN: BigDecimal::ROUND_HALF_EVEN,
  ROUND_CEILING: BigDecimal::ROUND_CEILING,
  ROUND_FLOOR: BigDecimal::ROUND_FLOOR,
}

# n = 2, 1, 0 over exact halves with either sign: no rounding lands on zero.
["123.456", "-123.456", "2.5", "-2.5", "1.25", "-1.25", "1.125", "-1.125"].each do |s|
  b = BigDecimal(s)
  [2, 1, 0].each do |n|
    MODES.each_value { |m| p b.round(n, m) }
  end
end

# n = -1 (to the tens place); magnitudes above 5, so no mode rounds to zero.
["123.456", "-123.456", "12.5", "-12.5", "125", "-125"].each do |s|
  b = BigDecimal(s)
  MODES.each_value { |m| p b.round(-1, m) }
end

# The Symbols CRuby accepts, on both signs at n = 0 and n = 1.
[:up, :down, :truncate, :half_up, :half_down, :half_even, :banker, :ceiling, :ceil, :floor, :default].each do |sym|
  p BigDecimal("2.5").round(0, sym)
  p BigDecimal("-2.5").round(0, sym)
  p BigDecimal("1.25").round(1, sym)
  p BigDecimal("-1.25").round(1, sym)
end

# Far below the unit: CRuby's base-1e9 array has no group at the rounding
# position, so every mode but CEILING/FLOOR truncates to zero and those two
# answer the unit. Only the positive zero and the FLOOR-of-a-negative are
# representable, so those are the shapes asked (the others are -0.0, which
# #round refuses).
p BigDecimal("0.09").round(-1, BigDecimal::ROUND_UP)
p BigDecimal("0.09").round(-1, BigDecimal::ROUND_HALF_UP)
p BigDecimal("0.09").round(-1, BigDecimal::ROUND_CEILING)
p BigDecimal("0.09").round(-1, BigDecimal::ROUND_FLOOR)
p BigDecimal("0.09").round(-2, BigDecimal::ROUND_CEILING)
p BigDecimal("-0.09").round(-1, BigDecimal::ROUND_FLOOR)
p BigDecimal("9.9").round(-10, BigDecimal::ROUND_UP)
p BigDecimal("9.9").round(-10, BigDecimal::ROUND_CEILING)
p BigDecimal("9.9").round(-10, BigDecimal::ROUND_FLOOR)
p BigDecimal("-9.9").round(-10, BigDecimal::ROUND_FLOOR)
p BigDecimal("1e-20").round(17, BigDecimal::ROUND_UP)
p BigDecimal("1e-20").round(17, BigDecimal::ROUND_CEILING)
p BigDecimal("1e-20").round(17, BigDecimal::ROUND_FLOOR)
p BigDecimal("1e-20").round(18, BigDecimal::ROUND_UP)
p BigDecimal("-1e-20").round(17, BigDecimal::ROUND_FLOOR)
p BigDecimal("-1e-20").round(18, BigDecimal::ROUND_UP)
p BigDecimal("-1e-20").round(18, BigDecimal::ROUND_FLOOR)

# No mode: no argument and n <= 0 answer an Integer, n > 0 a BigDecimal.
p BigDecimal("123.456").round
p BigDecimal("123.456").round(0)
p BigDecimal("123.456").round(-1)
p BigDecimal("-123.456").round
p BigDecimal("-123.456").round(-1)
p BigDecimal("123.456").round(1)
p BigDecimal("123.456").round(5)
p BigDecimal("2.5").round
p BigDecimal("-2.5").round

# A positive value rounding to zero is +0.0, exactly as CRuby's.
p BigDecimal("0.04").round(1)
p BigDecimal("0.4").round
p BigDecimal("0.4").round(0, BigDecimal::ROUND_FLOOR)
p BigDecimal("-0.4").round(1)
p BigDecimal("-0.4").round(0, BigDecimal::ROUND_FLOOR)
p BigDecimal("-0.4").round(0, BigDecimal::ROUND_UP)

# Zero keeps its (positive) sign, whatever the mode.
p BigDecimal("0").round
p BigDecimal("0").round(0, BigDecimal::ROUND_FLOOR)
p BigDecimal("0").round(2, BigDecimal::ROUND_CEILING)
p BigDecimal("0").round(-1, BigDecimal::ROUND_UP)

# An invalid mode is refused, not defaulted.
[0, 8, 99].each do |m|
  begin
    BigDecimal("1.5").round(1, m)
    puts "no raise"
  rescue ArgumentError => e
    puts "ArgumentError: #{e.message}"
  end
end
[:half_odd, :bogus].each do |m|
  begin
    BigDecimal("1.5").round(1, m)
    puts "no raise"
  rescue ArgumentError => e
    puts "ArgumentError: #{e.message}"
  end
end
