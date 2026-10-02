# Method#call already passes the later keyword value as a shared handle.
def kw(k:) = k << "x"
s = +"s"
method(:kw).call(k: +"z", k: s)
p s
