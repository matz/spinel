# spinel: int64
# Range#sum of a Range with more than 2**30 elements answers its whole sum.
# The sum came from an Array of the elements, which stops at 2**30 of them,
# so every long Range answered the sum of its first 2**30 elements.
p (1..2_147_483_648).sum
n = 3_000_000_000
p (1..n).sum
p (1...n).sum
p (1..n).sum(5)
p (1..n).sum(0.5)
p (-n..n).sum
p (-n..n - 1).sum
p (1..n).step(3).sum
p 1.step(n, 7).sum
r = [1, (1..n)].last
p r.sum
p r.sum(2)
p (n..1).sum
p (1..10).sum
p (4..4).sum(2)
