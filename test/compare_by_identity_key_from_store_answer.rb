# An identity Hash keyed by the String a store answers (#11078; was
# refused).
Sig = Struct.new(:name)
h = { a: Sig.new("a") }
r = (h[:b] = "str")
o = {}.compare_by_identity
o[r] = 1
p o.size
