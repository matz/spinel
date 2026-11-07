# A Hash's iteration hands its String keys on.
Sig = Struct.new(:name)
h = { "s" => Sig.new("a") }
o = {}.compare_by_identity
h.each { |k, v| o[k] = 1 }
p o.size
