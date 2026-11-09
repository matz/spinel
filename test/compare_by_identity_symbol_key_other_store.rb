# An identity Hash keyed by a read under :sig, beside a store under a key
# the compiler does not know (#11078; was refused).
Sig = Struct.new(:name)
def g(k)
  h = { sig: Sig.new("a") }
  h[k] = "str"
  o = {}.compare_by_identity
  o[h[:sig]] = 1
  o.size
end
p g(:sig)
