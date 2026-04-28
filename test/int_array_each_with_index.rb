# Array#each_with_index on an int_array. The element block param must
# shadow an outer same-named local of a different C type, and the
# index block param must be block-local mrb_int (not leak the outer
# `j`). Body skips reading the params to keep this scope-shadow case
# separate from the format-specifier path.

i = "hi"
j = "ho"
puts i
puts j
foo = [10, 20, 30]
n = 0
foo.each_with_index { |i, j| n = n + 1 }
puts n   # 3
