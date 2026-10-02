# A fresh String stored in a Hash must not silently lose its append.
n = 1
h = {k: "h#{n}"}
h.each_value { |x| x << "!" }
p h
