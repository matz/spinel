# Class.new(Hash) is a subclass of Hash spelled as a call (#7075).
Registry = Class.new(Hash)
r = Registry.new
r[:a] = 1
p r.size
