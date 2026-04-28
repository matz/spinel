# Array#concat on a poly_array used to silently miss the type-check and
# the loop never ran, so the receiver kept its original length.

a = [1, "x"]
a.concat([2, "y"])
puts a.length
