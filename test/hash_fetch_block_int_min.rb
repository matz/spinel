# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Hash#fetch with a block that answers -2**63 on a miss, on an Integer-keyed
# and on a String-keyed Integer Hash, a default proc that stores -2**63, and
# fetch through a nullable key: the answer is a number, never a miss.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
h = { 1 => 2 }
p h.fetch(1) { m }
p h.fetch(9) { m }
p h.fetch(9) { m }.nil?
p h.fetch(9) { |k| m + k }
p h.fetch(9) { |k| m }.zero?
p(h.fetch(9) { m } ? "truthy" : "falsy")
puts "#{h.fetch(9) { m }}"
p h.fetch(9) { m } + 1
p h.fetch(9) { m } == m
p [h.fetch(9) { m }].min
s = { "a" => 1 }
p s.fetch("a") { m }
p s.fetch("z") { m }
p s.fetch("z") { |k| k.size + m }
p s.fetch("z", m)
p s.fetch("z", m).nil?
d = Hash.new { |hash, key| hash[key] = m }
p d[1]
p d[1].nil?
p d
p d.fetch(1)
p d.size
e = Hash.new(m)
p e.fetch(5, 0)
p e[5]
p e.fetch(5) { 0 }
k = [1, 2].index(9)
p h.fetch(k) { m }
p h.fetch(k, m)
p s.fetch(k.to_s) { m }
j = [1, 2].index(1)
p h.fetch(j + 1) { m }
p h.fetch(j) { m }
begin
  p h.fetch(9)
rescue KeyError => ex
  puts "KeyError: #{ex.message}"
end
p h.fetch(9, nil)
p h.fetch(9, nil).nil?
p h.key?(9)
p h.fetch(9) { nil }
p h.fetch(9) { nil }.nil?
p h.values_at(1, 9)
p h.fetch_values(1) { m }
p h.fetch_values(9) { m }
p h.fetch_values(1, 9) { |k| m }
