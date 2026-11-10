# A Method of a boxed receiver (a Float in the default mode, or a poly value)
# is equal to another Method of the same method on the same receiver: ==, eql?
# and hash agree, and a Hash key finds it. Each `.method(:sym)` on such a
# receiver carries it in a holder of its own, so the holder is not what is compared.
f = 1.5
p f.method(:+) == f.method(:+)
p f.method(:+).eql?(f.method(:+))
p f.method(:+).hash == f.method(:+).hash
p f.method(:+) != f.method(:+)
# another receiver, or another method, is another Method
p f.method(:+) == 2.5.method(:+)
p f.method(:+) == f.method(:-)
# a Hash key
h = { f.method(:+) => :plus }
p h[f.method(:+)]
p h[f.method(:-)]
p h[2.5.method(:+)]
# an unbound copy
u = f.method(:+).unbind
p u == f.method(:+).unbind
p u == f.method(:+)
# a value that is poly in this program
x = [1, "a", 2.5].last
p x.method(:+) == x.method(:+)
p x.method(:+) == x.method(:-)
y = [1, "a", 2.5].first
p y.method(:+) == y.method(:+)
p y.method(:+) == x.method(:+)
# a receiver that is its own object is compared by identity
s = [1, "abc"].last
p s.method(:upcase) == s.method(:upcase)
p s.method(:upcase) == "abc".dup.method(:upcase)
