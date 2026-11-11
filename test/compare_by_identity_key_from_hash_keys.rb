# An identity Hash keyed by the String keys of another Hash finds them by
# equal?, as CRuby does (#11078; was refused).
Sig = Struct.new(:name)
h = { "s" => Sig.new("a") }
o = {}.compare_by_identity
o[h.keys.first] = 1
p o.size
