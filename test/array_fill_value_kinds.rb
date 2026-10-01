# Array#fill writes a value its array's element type cannot hold into a
# general Array, the block form as the value form already did: a String block
# value went into an Integer array's slot and the C did not build. A Float
# into an Integer array, or an Integer into a Float one, is not the same
# element either: the literal forms truncated 1.5 to 1 and printed 1 as 1.0.
b = [1, 2, 3]
b.fill { |i| "s#{i}" }
p b
p [1, 2, 3, 4, 5].fill(8, 5) { |i| "a" }
p [1, 2, 3, 4, 5, 6].fill(-4..4) { |i| (i + 1).to_s }
p [1, 2].fill(1.5)
p [1.5, 2.5].fill(1)
p [1, 2].fill { |i| i * 0.5 }
c = [1, 2]
c.fill { |i| i * 0.5 }
p c
p [1, 2, 3].fill("x", 1)
p [1, 2, 3].fill { |i| i + 1 }

# a `&` argument that is not a Proc may be nil, and then the first argument
# is the value
p [1, 2, 3].fill(2...2, &@nothing)
