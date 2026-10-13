# spinel: share
# A new String the runtime makes for a boxed receiver -- a case conversion,
# a concatenation, a repetition -- is boxed as its handle, so the holders
# the box is copied into name the one String and a change in place through
# one shows through the others. Stored into a String-valued Hash it is
# still that Hash's own copy.
def pick(x) = x
x = [+"ab", 1].first
$g = x.downcase
z = $g
z << "!"
p $g, z

h = { a: x * 2 }
v = h[:a]
v.upcase!
p h, v

S = Struct.new(:v)
o = S.new(x + "y")
q = o.v
q.replace("zz")
p o.v, q

g = pick(1) == 1 ? { "k" => "x" } : [1]
g["k"] += "y"
p g
