# Array#reject on an int_array. The block param must shadow an outer
# same-named local of a different C type. Body uses a constant
# predicate so this isolates the shadow case from the separate
# type-inferer behavior when reading the param inside the block.

i = "hi"
puts i
foo = [10, 20, 30]
none = foo.reject { |i| true }
puts none.length  # 0
all = foo.reject { |i| false }
puts all.length   # 3
puts all[0]       # 10
puts all[1]       # 20
puts all[2]       # 30
