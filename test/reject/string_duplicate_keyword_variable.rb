# This route would append to a copy of the caller's String.
def kw(k:) = k << "x"
s = +"s"
kw(k: +"z", k: s)
p s
