# spinel: gc-stress
# String#append_as_bytes with several arguments appends one at a time, each
# step answering a new String held in a C temp while the next argument is
# made. An Integer argument's byte, or an argument that is itself a new
# String, allocated there: under SPINEL_GC_STRESS=2 the collection freed the
# String built so far.
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
