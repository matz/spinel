# This route would append to a copy of the caller's String.
a = +"a"
((x, y), z), w = [[a, 1], 2], 3
x.upcase!
p a
