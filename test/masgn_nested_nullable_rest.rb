# A multiple assignment nested in another's values (a parenthesized
# statement list) leaves the outer one's nil-able values intact: a nil
# index answer stays nil in the outer splat rest.
ia = [1, 2, 3]
head, *rest = 0, ia.index(9), (x, y = ia.index(8), 2; "s")
p head, rest, x, y
a, b, *c = ia.index(7), (m, *n = 5, ia.index(1), ia.index(6); m), ia.index(2)
p a, b, c, m, n
# ... and a nested one in a block among the values
h3, *r3 = ia.index(9), [4].map { |v| a1, b1 = v, ia.index(v); a1 }
p h3, r3
