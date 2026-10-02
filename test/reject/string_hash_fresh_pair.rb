# A fresh String stored in a Hash must not silently lose its append.
h = {k: +"h"}
h.each_pair { |k, x| x << "!" }
p h
