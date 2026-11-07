# A store under a key the compiler does not know may be under :sig.
Sig = Struct.new(:name)
def g(k)
  h = { sig: Sig.new("a") }
  h[k] = "str"
  o = {}.compare_by_identity
  o[h[:sig]] = 1
  o.size
end
p g(:sig)
