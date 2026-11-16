# A multiple assignment into a splat rest of an Integer that may be nil
# (`*rest = ia.index(4), 7`, a block's find, `a.first` of an emptied Array)
# stores the value with its nil: into an Integer Array, into a Float Array
# (an Integer index beside a Float), and into a mixed rest, which boxes it.
ia = [3, 1, 4]; m = ARGV.size == 0 ? 99 : 4
*rest = ia.index(4), 7; p rest
*r2 = ia.index(m), ia.index(1); p r2
a, *r3 = 0, ia.index(m); p a, r3
*r4 = ia.index { |v| v == m }, 7; p r4
x = [1].tap(&:shift).first
*r5 = x, 2, x; p r5
fa = [1.5]; y = fa.index(9.9)
*r6 = y, 2.5; p r6
b, c, *r7 = ia.index(m), 1, 2, ia.index(3); p b, c, r7
