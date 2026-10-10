# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 as the value of a typed Integer-valued Hash and as a key: stored,
# read, missed, listed, summed; the miss is still nil.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
h = { 1 => 0, 2 => 0 }
h[2] = m
p h[2]
p h[2].nil?
p h[3]
p h[3].nil?
p(h[2] ? 1 : 0)
p(h[3] ? 1 : 0)
p(h[2] || 0)
p(h[3] || 0)
p h
puts "#{h[2]} #{h[3]}"
p h.values
p h.values.sum
p h.values.min
p h.key?(2), h.key?(3)
p h.value?(m)
p h.key(m)
p h.select { |k, v| v < 0 }
p h.count { |k, v| v != 0 }
p h.map { |k, v| v & 1 }
p h.min_by { |k, v| v }
p h.sort_by { |k, v| v }
p h.to_a
p h.invert
h.each { |k, v| p [k, v.nil?] }
h.each_value { |v| p v.nil? }
p h.values_at(1, 2, 3)
p h.dig(2)
p h.delete(2)
p h[2]
k = { 0 => 1 }
k[m] = 2
p k
p k.keys
p k[m]
p k.key?(m)
p k.keys.min
