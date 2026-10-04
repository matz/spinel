# `x.each(&pr)` on a receiver read out of a boxed slot answers the receiver,
# as `x.each { }` does. The call was typed nil, so its value was dropped:
# `p r.each(&q)` printed nil and `.class` on it raised.

q = proc { |x| x }
r = [1..2, 1][0]
p r.each(&q)
p r.each(&q).class
a = [[1, 2], 1][0]
p a.each(&q)
h = [{a: 1}, 1][0]
p h.each(&q)

# the proc still runs once per element
seen = []
w = [[3, 4], 1][0].each(&proc { |x| seen << x })
p w, seen
p r.each_with_index(&q)
