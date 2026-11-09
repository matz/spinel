# An identity Hash keyed by what is read under :sig, where a String may be
# stored under :sig (#11078; was refused).
Sig = Struct.new(:name)
h = {}
h[:sig] = Sig.new("a")
h[:sig] = "str" if ARGV.empty?
o = {}.compare_by_identity
o[h[:sig]] = 1
p o.size
