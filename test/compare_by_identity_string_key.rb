# An identity Hash with String keys finds a key by equal?, as CRuby does:
# a literal is one object per content in both (frozen string literals),
# and a String made afresh is a new object in both (#11078; was refused,
# when Symbol#to_s and -@ answered other objects than CRuby's).
h = Hash.new.compare_by_identity
h[[1, 2]] = 1
h["a"] = 2
p h.size
