# Array#each_with_object on an int_array. The element block param
# must shadow an outer same-named local of a different C type, and
# the accumulator block param must take the C type of the seed
# (here mrb_int) — not leak the outer `acc` (string). Body avoids
# reading the params to keep this scope-shadow case separate from
# the separate type-inferer behavior when reading the param inside
# the block.

i = "hi"
acc = "ho"
puts i
puts acc
foo = [10, 20, 30]
n = 0
foo.each_with_object(0) { |i, acc| n = n + 1 }
puts n   # 3
