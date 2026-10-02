# A fresh String stored in a Hash must not silently lose its append.
h = {}
h.store(:k, +"h")
h.each_pair { |k, x| x << "!" }
p h
