# A store answers what it stored, a String here.
Sig = Struct.new(:name)
h = { a: Sig.new("a") }
r = (h[:b] = "str")
o = {}.compare_by_identity
o[r] = 1
p o.size
