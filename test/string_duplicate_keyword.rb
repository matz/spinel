# Repeated keywords still bind the later value when it is fresh or not appended.
def kw(k:) = (p k; k)
kw(k: "a", k: "b")
def kw2(k:) = k << "x"
u = +"u"; kw2(k: u, k: +"z"); p u
