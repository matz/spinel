# Struct.new(*keys) where the program changes the Array in keys after its one
# write (`keys << :c`, `keys.sort!`, `add(keys)`, `other = keys`): the names
# the write made are not the names Struct.new reads, so the member list is
# known only at run time and refused. Spinel built a Struct of [:a, :b].
keys = %i[a b]
keys << :c
R = Struct.new(*keys)
p R.members
