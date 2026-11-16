# A String JSON.parse makes is one String: a change through the Array or Hash that holds it shows there.
require "json"
a = JSON.parse('["a","b"]')
a[0] << "!"
p a
h = JSON.parse('{"k":["v"],"s":"w"}')
h["k"][0] << "?"
h["s"] << "?"
p h
x = a[1]
x << "#"
p a, x.equal?(a[1])
p JSON.parse('"top"') << "!"
puts JSON.generate(a)
