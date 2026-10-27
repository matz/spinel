# equal? on a boxed Float answers what a typed Float's does: a Float is held
# by value, so its identity is its bits. The same NaN is equal? to itself,
# and 0.0 is not -0.0 (CRuby's 0.0 is a flonum, its -0.0 a heap Float).
n = Float::NAN
z = 0.0
mz = -0.0
p [n.equal?(n), z.equal?(mz), z.equal?(0.0), 2.5.equal?(2.5)]

a = [Float::NAN, 0.0, -0.0, 2.5, :k, "s"]
p [a[0].equal?(a[0]), a[0].equal?(n), a[1].equal?(a[2]), a[2].equal?(a[1])]
p [a[1].equal?(a[1]), a[3].equal?(2.5), a[3].equal?(2.25), a[0].equal?(a[3])]
p [a[1].equal?(0), a[4].equal?(:k), a[0] == a[0], a[1] == a[2]]

h = { x: Float::NAN, y: -0.0 }
p [h[:x].equal?(n), h[:y].equal?(h[:y]), h[:y].equal?(0.0)]

# the same NaN in an Array is found by identity where == fails
p [[n].include?(n), a.index { |e| e.equal?(n) }, a.count { |e| e.equal?(0.0) }]
