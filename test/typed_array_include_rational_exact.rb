# spinel: int64
# A Rational and an Integer compare exactly: 9007199254740992r is not
# 9007199254740993, though both are the same Float.
def g(a, x) = a.include?(x)
def gi(a, x) = a.rindex(x)
p [9007199254740993].include?(9007199254740992r)
p [9007199254740992].include?(9007199254740992r)
p [9007199254740993].index(9007199254740992r)
p g([9007199254740993], 9007199254740992r), g([1, 2], 2r), g([1, 2], 5r / 2), gi([2, 3, 2], 2r)
p g([1.0, 2.0], 2r), [1.0, 2.5].index(5r / 2)
b = [[9007199254740993], "s"]
p b[0].include?(9007199254740992r)
p 9007199254740993 == [9007199254740992r, "s"][0]
def f(x) = x
r = f(Rational(9223372036854775807, 1))
big = f(2**63)
p r == big, big == r
p [r, "s"].include?(big), [big, "s"].include?(r)
p f(Rational(2**64, 1)) == f(2**64)
