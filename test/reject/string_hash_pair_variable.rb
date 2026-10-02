# A String variable on this route must not silently lose its append.
s = +"a"
h = {}
h[:k] = s
h.each_pair { |k, q| q.concat("!") }
p s, h
