# A String variable on this route must not silently lose its append.
s = +"a"
h = {k: s}
h.each_value { |q| q << "!" }
p s, h
