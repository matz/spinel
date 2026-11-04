# A typed array passed to a parameter the method mutates, where the
# argument comes back through a Struct member that kept the array its
# constructor was handed: `src`, the member and the parameter hold one
# Array, so they take one kind and the append reaches `src` (CRuby prints
# [1, 2, "z"] twice). It was refused at compile time (#4480), since the
# parameter's general Array copied the argument; the names of one
# allocation now share its kind (#10930).
Box = Struct.new(:a)
def add(out) = out << "z"
src = [1, 2]
x = Box.new(src).a
add(x)
p src, x
