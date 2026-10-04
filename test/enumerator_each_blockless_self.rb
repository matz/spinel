# Enumerator#each without a block answers the Enumerator itself.
e = [1, 2].each
p e.each.to_a
p e.each.equal?(e)
f = [3, 4].map
p f.each.to_a
g = (1..3).each
p g.each.to_a
h = [5, 6].each_with_index
p h.each.to_a
p e.each { |x| x }.class
x = e.each.each
p x.next, x.next
e.each
p e.each.map { |v| v * 3 }
