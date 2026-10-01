# nil as a key of an Integer-keyed Hash. A key written from an Integer
# slot that held nil is stored as that slot's sentinel, and the Hash shows
# it as nil (inspect, keys, fetch with a default, dig); key?, include?,
# member?, [], fetch and values_at took a nil key as one the table cannot
# hold, and answered false, nil or a KeyError.
x = 1
p x
x = nil
g = {}
g[x] = 1
g[2] = 3
p g, g.keys
p g.key?(nil), g.include?(nil), g.member?(nil), g.has_key?(nil)
p g[nil], g.fetch(nil), g.fetch(nil, 9), g.dig(nil)
p g.values_at(nil, 2, 5)
p g.delete(nil), g
p g.key?(nil), g[nil]
begin
  g.fetch(nil)
rescue KeyError => e
  p e.message
end

# a table no nil key was written to still misses
h = { 1 => 2 }
p h[nil], h.key?(nil), h.fetch(nil, :d), h.values_at(nil)
