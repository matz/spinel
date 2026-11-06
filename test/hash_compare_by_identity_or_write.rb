# A compare_by_identity Hash whose slot holds a boxed value (`h[k] ||= {}`
# widens it): the call answers its receiver, a Hash, so the slot keeps the
# Hash and not nil (#11080).
S = Struct.new(:name)
a = S.new("f")
b = S.new("f")
h = {}.compare_by_identity
h[a] ||= {}
h[a][:x] = 1
h[b] ||= []
h[b] << 2
p h.size, h[a], h[b], h.compare_by_identity?

g = Hash.new.compare_by_identity
g[a] ||= {}
p g.size, g.key?(b)
