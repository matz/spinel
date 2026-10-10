# spinel: int64 -- the loop bounds are the ends of a 64-bit Integer
# An Integer loop whose limit is 2**63-1 or -2**63 stops there: the end is
# tested before the step, so the walk never steps past the last value of
# sp_int (it overflowed, wrapped, and ran again). upto / downto, Integer#step
# with a block and as an Array, a Range's each, and a receiver of run-time
# type under --int-overflow=promote.
n = ARGV.size
x = 9223372036854775807 - n
c = 0
(x - 1).upto(x) { |v| c += 1; break if c > 5 }
p c
d = 0
(x - 1..x).each { |v| d += 1; break if d > 5 }
p d
y = -9223372036854775808 + n
e = 0
(y + 1).downto(y) { |v| e += 1; break if e > 5 }
p e
f = 0
(x - 1).step(x, 1) { |v| f += 1; break if f > 5 }
p f
g = 0
(y + 1).step(y, -1) { |v| g += 1; break if g > 5 }
p g
c = 0
(9223372036854775806 - n).upto(9223372036854775807) { |v| c += 1; break if c > 5 }
p c
c = 0
(-9223372036854775807 + n).downto(-9223372036854775808) { |v| c += 1; break if c > 5 }
p c
c = 0
(9223372036854775806 - n).step(9223372036854775807) { |v| c += 1; break if c > 5 }
p c
c = 0
(9223372036854775805 - n).step(9223372036854775807, 2) { |v| c += 1; break if c > 5 }
p c
c = 0
x = 9223372036854775807 - n
(x - 1).upto(x) { |v| c += 1; break if c > 5 }
p c
c = 0
(x - 1..x).each { |v| c += 1; break if c > 5 }
p c
c = 0
2.times { |i| c += 1 }
p c
p (9223372036854775805 - n).step(9223372036854775807, 2).to_a.size
p (-9223372036854775806 + n).step(-9223372036854775808, -1).to_a.size
p 1.step(10, 3).to_a, 10.step(1, -4).to_a
