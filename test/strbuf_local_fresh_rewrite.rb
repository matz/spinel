# A String local that is the shared handle, written again with a fresh
# String (`+"lit"`, a `dup`) after a write the alias analysis demanded:
# the new value is wrapped as a new handle, as any fresh String stored
# into the slot is. It was handed over as a plain String, and the C did
# not build.

X = "x"
x = [1, +"q"][1]
t = x.is_a?(String) ? x : +"n"
t = +"Ab"; t << X; p t
u = (x if x.is_a?(String))
u = "Cd".dup; u << X; p u
v = x.is_a?(String) ? x : +"n"
w = +"Ef"
v = w; v << X; p v, w
y = x.is_a?(String) ? x : +"n"
p y
y = +"Gh"; p y
