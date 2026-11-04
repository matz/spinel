# spinel: int64
# A Bignum compared with a Rational, or with a boxed value of any class,
# compares by value (it read a boxed Rational as 0).
b = 2**64
r = Rational(2**64, 1)
h = Rational(2**65 + 1, 2)
p b == r, r == b, b != r, b < h, b > h, b <= r, b >= r, b == h
z = [r, 1].first
p b == z, b < z, b > z, z == b, b != z
f = [2**64, 1].first
p f == r, f == 2**64
p b == 2.0**64, b == [2.0**64, 1].first
p b == [b, "s"].first, b == ["s", 1].first, b != [nil, 1].first
p b == [1.5, 2].first, b > [1.5, 2].first
p b <=> r
p [b].include?(r)
def cmp(a, b) = [a == b, a != b, a < b, a >= b]
p cmp(2**70, Rational(2**71, 2))
p cmp(2**70, [Rational(2**70 + 1, 1), :x].first)
p cmp(-(2**70), [3r, "s"].first)
p cmp(2**64, [2**64 + 1, nil].first)
# The boxed side first, a program object's ==, nil, a Float, an Integer:
z2 = [Rational(2**64, 1), 1].first
p z2 < b, z2 <= b, z2 > b, z2 >= b, z2 != b
class M; def ==(o) = :yes; def coerce(o) = [o, 2**64]; end
m = [M.new, 1].first
p b == m, b != m
q = [nil, 1].first
p b == q, b != q
begin; p b < q; rescue => e; p e.class; end
w = [3.0e30, 1].first
p b < w, w > b, b == [18446744073709551616.0, 2].first
x = [2**64 + 1, 2].first
p b < x, x > b, x == b + 1
