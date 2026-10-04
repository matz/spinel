# A lambda or proc whose own body uses no numbered parameter takes no
# argument for the `_1` or `it` of a block nested in it; one that does keeps
# its count, and a lambda nested in another counts its own.
p -> { [1, 2].map { _1 * 2 } }.call
p proc { [1, 2].map { _1 * 2 } }.call
f = -> { [3].map { it + 1 } }
p f.call
g = -> { [[1, 2]].map { _1 + _2 } }
p g.call
def m = yield
p m { [1, 2].map { _1 * 3 } }
h = -> { _1 + [1, 2].map { |x| x * 10 }.sum }
p h.call(5)
k = -> { [it, [7].map { |y| y + 1 }] }
p k.call(3)
p -> { 3.times.map { _1 } }.arity, -> { _1 + _2 }.arity
q = -> { -> { _1 * 2 }.call(4) }
p q.call
