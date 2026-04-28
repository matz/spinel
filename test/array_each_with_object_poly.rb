# Array#each_with_object on a poly_array used to silently miss the
# type-check; the loop body never ran.

n = 0
[1, "a", :sym].each_with_object("") {|_elem, _acc| n += 1 }
puts n
