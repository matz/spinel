# Appending to the source and to a fresh sibling keeps the supported paths.
b = +"b"
(m, n), o = [b, 1], 2
b << "?"
p m
q = +"q"
(r1, r2), r3 = [q, +"k"], 2
r2 << "z"
p q, r2
