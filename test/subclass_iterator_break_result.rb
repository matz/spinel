# A builtin iterator on a subclass can answer its receiver or a block break.
# The subclass adapter surrounds only the normal result, not the break box.
class BreakArray < Array
end
class BreakHash < Hash
end
def array_walk(a, stop)
  a.each { break "array stop" if stop }
end
def hash_walk(h, stop)
  h.each { break "hash stop" if stop }
end
a = BreakArray.new([1, 2])
h = BreakHash.new
h["x"] = 1
p array_walk(a, false).equal?(a), array_walk(a, true)
p hash_walk(h, false).equal?(h), hash_walk(h, true)
