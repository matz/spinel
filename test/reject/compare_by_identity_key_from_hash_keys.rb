# The keys of a Hash are Strings here.
Sig = Struct.new(:name)
h = { "s" => Sig.new("a") }
o = {}.compare_by_identity
o[h.keys.first] = 1
p o.size
