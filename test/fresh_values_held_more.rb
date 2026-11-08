# spinel: gc-stress
# More new objects lost to the next allocation, found running the test
# corpus under SPINEL_GC_STRESS=2.

# a String Range's minmax: the pair's Array, while each element is made
def sr(a, b) = (a..b)
def ss(a, b) = (a..b)
p sr(:a, :e).minmax, ss("b", "d").minmax
e = [:e, :f][ARGV.size]
p (:a..e).minmax

# a String inserted into an Array of Strings: its handle, while insert grows
a = %w(p q)
a.insert(1, "x" + "y")
a.unshift("u" + "v")
p a, a.index("xy"), a.include?("uv")

# Hash.new(default).update(other): the new Hash, while each pair is made
h = Hash.new(0).update({ "a" => 1, "b" => 2 })
p h, h["z"]

# Range#overlap?: the receiver's box, while the argument's is made
p (1..3).overlap?(2.5..9)
def rr(x) = x ? (1..5) : 3
p rr([true][0]).overlap?(4..10)

# String#% with %p: the list made for it, while inspect allocates
fl = [4.5]
fl << [1.5, 2.5][ARGV.size + 7]
p "%s|%p" % fl
p format("%p %p", [1, nil], "s")
