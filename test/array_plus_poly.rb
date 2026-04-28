# Array#+ on a poly_array used to fall through to the C `+` operator
# applied to two `sp_PolyArray *` pointers — a compile error.

a = [1, "x"]
b = [2, "y"]
c = a + b
puts c.length
