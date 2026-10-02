# This route would append to a copy of the caller's String.
def m(k1:) = k1 << "x"
v = +"v"
method(:m).call(**{ k1: v })
p v
