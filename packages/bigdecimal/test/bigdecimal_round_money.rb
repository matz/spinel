# The money shapes a Rails app writes (#4881): a cents rate times hours,
# rounded half up to an Integer; seconds turned into hours by division; and
# a price string scaled by 100.
require "bigdecimal"

rate_cents = 1999
hours = 3
p (BigDecimal(rate_cents.to_s) * hours).round(0, BigDecimal::ROUND_HALF_UP)

seconds = 7260
# The quotient is compared after an exact round(4): CRuby picks its own
# division precision, the package carries DIV_DIGITS, and only the rounded
# value is guaranteed to agree.
p (BigDecimal(seconds.to_s) / 3600).round(4)
p (BigDecimal(seconds.to_s) / 3600).to_f

p (BigDecimal("38.40") * 100).round
p (BigDecimal("38.40") * 100).round.to_i
p (BigDecimal("0.005") * 100).round(0, BigDecimal::ROUND_HALF_UP)

# A negative refund, rounded half up away from zero and half even to a tie.
p (-BigDecimal("12.345")).round(2, BigDecimal::ROUND_HALF_UP)
p (-BigDecimal("12.345")).round(2, BigDecimal::ROUND_HALF_EVEN)
