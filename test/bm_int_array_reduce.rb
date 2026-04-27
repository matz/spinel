# Array#reduce on an int_array. The element block param must shadow
# an outer same-named local of a different C type.

i = "hi"
puts i
foo = [1, 2, 3, 4, 5]
sum = foo.reduce(0) { |acc, i| acc + i }
puts sum   # 15
