# spinel: int64
# random_number over a positive Integer bound: the one-time-code shape
# (`SecureRandom.random_number(10**6)`).
require "securerandom"

# Always inside 0...n, across bounds that need one, two, three, six, seven
# and eight bytes, at and around each byte boundary.
[1, 2, 10, 255, 256, 257, 1000, 10**6, 2**40, 2**56, 2**56 + 1, 2**62 + 3].each do |n|
  ok = true
  200.times do
    v = SecureRandom.random_number(n)
    ok = false if v < 0 || v >= n
  end
  puts ok
end

# A bound of 1 has one answer.
puts SecureRandom.random_number(1)

# Every value of a small range turns up, and no value dominates: a modulo
# bias or a stuck high byte would show as a missing or lopsided bucket.
counts = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
20000.times { counts[SecureRandom.random_number(10)] += 1 }
puts counts.min > 1700
puts counts.max < 2300

# Successive six-digit codes differ.
puts SecureRandom.random_number(10**6) == SecureRandom.random_number(10**6) && SecureRandom.random_number(10**6) == SecureRandom.random_number(10**6)

# The draw reaches the top of a bound that is just past a byte boundary
# (257 needs two bytes, its top byte masked to one bit): every value of
# 0...257 turns up, so neither the mask nor the rejection cuts the range.
seen = {}
20000.times { seen[SecureRandom.random_number(257)] = true }
puts seen.size
