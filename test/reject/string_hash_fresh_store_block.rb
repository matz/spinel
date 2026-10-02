# A fresh String stored in a Hash must not silently lose its append.
h = {}
h.store(:k, +"h") { raise "not called" }
h.each_value { |x| x << "!" }
p h
