# The key is read under :sig, and a String may be stored under :sig.
Sig = Struct.new(:name)
h = {}
h[:sig] = Sig.new("a")
h[:sig] = "str" if ARGV.empty?
o = {}.compare_by_identity
o[h[:sig]] = 1
p o.size
