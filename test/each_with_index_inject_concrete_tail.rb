# An inject over each.with_index whose accumulator is boxed takes a block
# tail of a concrete type boxed: the tail does not read the accumulator.
p [1, 2].each.with_index.inject(0) { |sum, (v, i)| 1r }
p [1, 2].each.with_index.inject(0) { |sum, (v, i)| 2.5 }
p [1, 2].each.with_index.inject(0) { |sum, (v, i)| sum + v * 2**-i }
