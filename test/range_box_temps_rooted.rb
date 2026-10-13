# spinel: gc-stress
# A Range boxes into a new object. curry[range] applied one curry to the
# next inside one C expression, holding the curry so far only in the
# argument list while the Range was boxed, and fill(range) on an Array it
# rebuilt as a boxed one held the rebuilt Array the same way: under
# SPINEL_GC_STRESS=2 the box's allocation freed it.
R = ->(r, n) { r.include?(n) }
p R.curry[(1..9)][5], R.curry[1..3][7]
S = ->(a, b, c) { [a, b, c].map(&:to_s).join(",") }
p S.curry[1..2][3...4]["x"]
p [1, 2, 3].fill(2...2), [1, 2].fill(0..1, 1)
