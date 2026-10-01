# A blockless map, collect, select, filter or reject on a boxed Array (one
# read out of a Hash, or a local that holds an Array or a String) is an
# Enumerator, as it is on a typed Array, and `to_enum` there is the
# blockless each. Both were left untyped, so a chained `.each { }`
# raised "undefined method 'each' for unknown", and a stored `map` raised
# NoMethodError naming Array. Chained, stored and read back with to_a, over
# a boxed Array, Hash and Range.
h = { k: [3, 1] }
p h[:k].map.each { |q| q + 1 }
p h[:k].collect.each { |q| q * 2 }
p h[:k].select.each { |q| q > 1 }
p h[:k].filter.each { |q| q > 1 }
p h[:k].reject.each { |q| q > 1 }
p h[:k].to_enum.each { |q| q }
e = h[:k].map
p e.class
p e.to_a
f = h[:k].select
p f.each { |q| q > 1 }

x = 1
a = x > 0 ? [3, 1] : "s"
p a.to_enum.each { |q| q }
p a.map.each { |q| q * 3 }
g = a.map
p g.class
p g.each { |q| q - 1 }

hs = { k: { a: 1, b: 2 } }
p hs[:k].map.each { |k, v| v }
rs = { k: (1..3) }
p rs[:k].map.each { |q| q + 1 }
p rs[:k].select.each { |q| q.odd? }
