# spinel: int64
# Range#sum of a Range with more than 2**30 elements answers its whole sum.
# The sum came from an Array of the elements, which stops at 2**30 of them,
# so every long Range answered the sum of its first 2**30 elements.
p (1..2_147_483_648).sum
n = 3_000_000_000
p (1..n).sum
p (1...n).sum
p (1..n).sum(5)
p (-n..n).sum
p (-n..n - 1).sum
r = [1, (1..n)].last
p r.sum
p r.sum(2)
p (n..1).sum
p (1..10).sum
p (4..4).sum(2)
# A Float seed: the total does not pass through the word on its way. (CRuby
# adds a Float-seeded Range one element at a time, so these stay just past
# 2**30 elements.)
m = 1_100_000_000
p (1..m).sum(0.5)
p [1, (1..m)].last.sum(0.25)
