# spinel: gc-stress
# Earlier arguments stay live while the remaining arguments allocate.
# Each Integer contributes its low byte, including zero.
s = +""
s.append_as_bytes("a", "é", 0, "\xff".b, "")
p s.bytes, s.encoding
t = +"x"
i = 3
t.append_as_bytes("#{i}", 65, "y" * i, 66)
p t
u = +"q"
u.append_as_bytes(97, 98, 99)
p u
