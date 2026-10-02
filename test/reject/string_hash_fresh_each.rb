# A fresh String stored in a Hash must not silently lose its append.
h = {k: +"h"}
h.each { |k, x| x << "!" }
p h
