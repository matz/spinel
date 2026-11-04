# spinel: gc-stress
# These keys collide in bucket 15 of Spinel's initial 16-slot FNV table, so the
# cluster wraps to slot zero. Removing its head/middle/tail must repair probes
# without changing the insertion order or losing the removed mixed value.
keys = ["collision-3", "collision-10", "collision-29", "collision-43", "collision-65", "collision-76"]
h = {}
keys.each_with_index { |key, i| h[key] = i.even? ? i : "value#{i}" }
p h.delete(keys[0].dup), h.delete(keys[3].dup), h.delete(keys[5].dup)
p h.keys, h.values, h.size
p h[keys[1]], h[keys[2]], h[keys[4]], h.delete(keys[3])
h[keys[0]] = "reinserted"
p h.keys, h.values

# Force growth and repeated deletion/reinsertion. Check all surviving values,
# not only the length/order: back-shifting a collision must move its value too.
large = {}
80.times { |i| large["key#{i}"] = i.even? ? i : "value#{i}" }
80.times do |i|
  key = "key#{i}"
  value = large.delete(key.dup)
  expected = i.even? ? i : "value#{i}"
  raise "wrong removed value" unless value == expected
  raise "key survived deletion" if large.key?(key)
  large[key] = value
end
correct = true
80.times do |i|
  expected = i.even? ? i : "value#{i}"
  correct = false unless large["key#{i}"] == expected
end
p correct, large.size, large.keys.first(3), large.keys.last(3)

# Keep the removed object alive across collections after it leaves the hash.
objects = { "keep" => ["kept"], "integer" => 1, "text" => "value" }
removed = objects.delete("keep")
GC.start
p removed, objects.keys
