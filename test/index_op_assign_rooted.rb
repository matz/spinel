# spinel: gc-stress
# `recv[key] op= value` on a boxed receiver reads the slot, applies the
# operator and stores back under the same key. A key whose boxing allocates
# (a Range) and an element read that is a new String (a String receiver's
# slice) were held only in C temps while the right-hand side and the
# operator allocated: under SPINEL_GC_STRESS=2 the store read a freed key.
r = [+"abcdef", 1][0]
r[0..1] += "-"
p r
r[1...3] += "#{r.size}"
p r
s = [+"xyz", 1][0]
s[0..1] += "<#{s}>"
p s
h = [{"k" => "v"}, 1][0]
h["k"] += "#{h.size}w"
p h
a = [["p", "q"], 1][0]
a[1] += "#{a.size}"
p a
