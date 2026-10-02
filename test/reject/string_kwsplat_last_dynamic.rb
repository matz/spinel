# The final splat supplies the String variable that would be copied.
def m(k:) = k << "x"
s = +"s"
p method(:m).call(k: +"other", **{ k: s })
p s
