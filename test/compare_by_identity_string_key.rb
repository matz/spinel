# spinel: gc-minor
# An identity Hash with String keys finds a key by equal?, as CRuby does:
# a literal is one object per content in both (frozen string literals),
# and a String made afresh is a new object in both (#11078; was refused,
# when Symbol#to_s and -@ answered other objects than CRuby's).
h = Hash.new.compare_by_identity
h[[1, 2]] = 1
h["a"] = 2
p h.size

key = +"key"
h[key] = 3
stored = h.keys[2]
key << "!"
p [stored.equal?(key), stored.frozen?, h[key], h[+"key!"], stored]
p h.delete(key)

ordinary = {}
ordinary[key] = 4
copy = ordinary.keys[0]
key << "?"
p [copy.equal?(key), copy.frozen?, ordinary[copy], ordinary[key]]

# A static String can reach the hash directly or through a boxed slot.
literal = "alpha"
boxed = [literal, :other]
literals = {}.compare_by_identity
literals[literal] = :first
p [literal.equal?("alpha"), literals["alpha"], literals[boxed[0]]]
literals["alpha"] = :second
p [literals.size, literals[literal], literals[boxed[0]]]
