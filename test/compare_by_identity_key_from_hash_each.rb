# An identity Hash keyed by the String keys a Hash's iteration hands on
# finds them by equal?, as CRuby does: the keys are CRuby's objects here
# too (#11078; was refused).
Sig = Struct.new(:name)
h = { "s" => Sig.new("a") }
o = {}.compare_by_identity
h.each { |k, v| o[k] = 1 }
p o.size
