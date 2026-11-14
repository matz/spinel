# spinel: int64
# spinel: gc-stress
# A Complex whose Integer part is past 2**63 boxes that part as a new
# Bignum when it is read. A boxed Complex's rect / rectangular held both
# parts in C locals and rooted the real part only after the imaginary part
# was made, and neither before the result Array was allocated: under
# SPINEL_GC_STRESS=2 that collection freed a part. Complex == a boxed
# number also compares against a new Bignum real part, now rooted.
z = Complex(1, 2) * 2**62 * 8
w = [z, "s"].first
p w.rect
p w.rectangular
p w.rect.map(&:class)
c = Complex(1, 0) * 2**62 * 4
q = [c, "s"].first
p q == 2**64
p q == 2**64 + 1
p q == 7
x = [2**64, "s"].first
p x == c
p c == x
p [Complex(3, 0), "s"].first == 3
