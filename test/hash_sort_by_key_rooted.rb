# spinel: gc-stress
# Hash#sort_by holds each entry's key, the block's value, until it is stored.
# The block's value is a temp whose own root ends with the block (a new
# Array, an interpolated String), and the [key, pair] tuple was allocated
# after the block ran: under SPINEL_GC_STRESS=2 that allocation freed the key
# before the push took it.
counts = Hash.new(0)
%w[the dog the cat a dog the].each { |w| counts[w] += 1 }
p counts.sort_by { |w, n| [-n, w] }
p counts.sort_by { |w, n| "#{n}#{w}" }.first(2)
h = {"b" => 2, "a" => 1, "c" => 3}
p h.sort_by { |k, v| [v.odd? ? 0 : 1, "#{k}"] }.map(&:first)
p h.sort_by { |k, v| -v }, h.sort_by { |k, _| k }
