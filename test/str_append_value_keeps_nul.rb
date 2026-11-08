# `s << t` used as a value keeps every byte of t, a NUL among them, as the
# statement form does (#11349)
head = +"H:"
out = head << "ab\x00cd"
p out.bytesize
p head.bytesize
p out.bytes
s2 = +"x"
p (s2 << "a\x00b".b).bytesize
body = [1, 0, 2].pack("C*")
resp = +"HTTP "
p (resp << body).bytes
t = "q\x00r".b
u = +"u"
v = u << t << "!"
p v.bytes
p v.encoding
def wrap(h, b) = h << b
p wrap(+"w", "1\x002").bytesize
