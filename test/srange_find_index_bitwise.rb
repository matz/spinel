# A String Range's find_index(v) in a program that reads an Integer with
# `& | ^`: a miss is nil, and nil & 1 is false, nil | 2 true, nil ^ 1 true;
# a hit is the Integer's.
def at(r, s) = r.find_index(s)
x = at(("a".."c"), "z")
p(x & 1, x | 2, x ^ 1)
y = ("a".."e").find_index("d")
p(y & 1, y | 8, y ^ 1)
w = ("a".."c").find_index(ARGV.size == 0 ? "c" : "q")
p(w.nil? ? -1 : w + 10)
R = ("a".."e")
p R.find_index("q"), (R.find_index("q") & 1), (R.find_index("d") & 1)
