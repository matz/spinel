# spinel: int64
# A Range whose ends are far apart, and Integer#[] over a Range, at the ends
# of the word: the width between the ends was a signed difference, which
# overflowed (a non-empty Range counted 0) or became an out-of-range shift.
n = ARGV.size
a = -5000000000000000000 - n
b = 5000000000000000000 + n

p (a..b).min
p (a..b).max
p (a..b).first(2)
p (0..-1 - n).step(2).size

v = 0b1011_0110 + n
lo = 2 - n
p v[lo..5]
p v[lo...5]
p v[lo..]
p v[lo..63]
p v[lo..64]
p v[lo..100]
p v[5 + n..2]
p v[3 + n..2]
p v[3 + n...3]
p v[-3 + n..2]
p v[70 + n..80]
p (-5 - n)[70..80]
p (-5 - n)[70..]
p v[-3 + n..]

w = [v, "x"][0]
p w[5 + n..2]
p w[3 + n...3]
p w[-3 + n..2]
p w[70 + n..80]
