# A fresh String stored in a Hash must not silently lose its append.
def fresh = +"h"
h = {k: fresh}
h.each_value { |x| x << "!" }
p h
