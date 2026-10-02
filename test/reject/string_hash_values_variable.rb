# A String variable on this route must not silently lose its append.
s = +"a"
h = {k: s}
h.values.each { |q| q << "!" }
p s, h
