# This route would append to a copy of the caller's String.
def run(v) = yield(**{ k1: v })
v = +"v"
run(v) { |k1:| k1 << "r" }
p v
