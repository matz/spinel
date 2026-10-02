# A stored String variable must be refused before the value block appends.
s = +"a"
h = {}
h.store(:k, s) { :ignored }
h.each_value { |v| v << "x" }
p s, h[:k]
