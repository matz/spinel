# Array#inject on an int_array. inject and reduce share
# compile_reduce_block, but inject has its own dispatch entry,
# so cover it independently in case the entry points diverge.
# Block param must shadow the outer same-named local.

i = "hi"
puts i
foo = [1, 2, 3, 4, 5]
sum = foo.inject(0) { |acc, i| acc + i }
puts sum   # 15
