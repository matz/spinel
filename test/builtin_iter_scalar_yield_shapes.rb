# A block given to each_index, fill, each_byte, step, inject with a seed,
# each_key, each_value or Array.new binds a rest, an optional, a post or a keyword by
# CRuby's rules, and so does the leading-requireds-and-rest block of an
# iterator whose emitter binds only the requireds.

a = [3, 1]
a.each_index { |*r| p r }
a.each_index { |i, *r| p [i, r] }
a.each_index { |i, j = 5| p [i, j] }
a.each_index { |*r, i| p [r, i] }
a.each_index { |i, k: 4| p [i, k] }
p(a.dup.fill { |*r| r.size })
p(a.dup.fill(1) { |i, j = 5| i + j })
p(a.dup.fill { |*r, i| i * 2 })
"ab".each_byte { |*r| p r }
"ab".each_byte { |b, c = 0| p [b, c] }
1.step(5, 2) { |*r| p r }
1.step(3, 2) { |i, j = 5| p [i, j] }
1.0.step(2.0, 0.5) { |*r, x| p [r, x] }
(1..4).step(2) { |i, k: 4| p [i, k] }
p(a.inject(0) { |*r| r.sum })
p(a.inject(0) { |s, *r| s + r[0] })
p(a.reduce(1) { |s, x, y = 2| s * x * y })
p([1.5, 2.5].inject(0.0) { |*r, x| r[0] + x })
h = { x: 1, y: 2 }
h.each_key { |*r| p r }
h.each_value { |v, w = 0| p [v, w] }
p(Array.new(2) { |*r| r })
p(Array.new(2) { |i, j = 5| [i, j] })

# a rest beside the requireds, which these emitters left nil or refused
"ab".each_char { |c, *r| p [c, r] }
(1..2).each { |i, *r| p [i, r] }
p(a.sort_by { |x, *r| [r, -x] })
p(a.sum { |x, *r| x + r.size })
p([3, 1, 3].uniq { |x, *r| x })
p(a.dup.select! { |x, *r| r.empty? && x > 1 })
p(a.dup.keep_if { |x, *r| x > 1 })
p(a.dup.delete_if { |x, *r| x > 1 })
p(a.dup.reject! { |x, *r| x > 1 })
p(a.dup.map! { |x, *r| x + r.size })
p(a.dup.sort_by! { |*r| r[0] })
p(a.dup.sort_by! { |x, y = 0| x + y })
