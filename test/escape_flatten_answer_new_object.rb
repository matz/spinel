# Regexp.escape with nothing to escape, and flatten of an Array with nothing
# to flatten (a frozen one too), answer a new object, not the one given.
p Regexp.escape("ab").frozen?
e = Regexp.escape("ab")
e << "c"
p e
flat = [1, 2].freeze
p flat.flatten.frozen?, flat.flatten.equal?(flat)
f = flat.flatten
f << 3
p f
a = ["x", "y"]
b = a.flatten
b << "z"
p a, b
fl = [1.5].freeze
g = fl.flatten
g << 2.5
p g, fl.frozen?
