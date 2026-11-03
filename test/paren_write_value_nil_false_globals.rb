# a parenthesized write value holding a nil global, then a false one, in one
# local: the load walk places the statements inside the parentheses
x = ($h = nil; $h)
p x
x = ($k = false; $k)
p x
