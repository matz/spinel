# `h.each_value.with_index { |v, i| }` binds a value and its Integer index.
# Left to the body, the value's `<<` typed it a String Array, the stored
# String was read as one, and the program crashed.
h = { k: +"e", j: +"f" }
h.each_value.with_index { |q, i| q << i.to_s; p q }
r = { a: "x", b: "y" }
r.each_value.with_index { |q, i| p [q, i] }
n = { a: 1, b: 2 }
n.each_value.each_with_index { |v, i| p v + i }
