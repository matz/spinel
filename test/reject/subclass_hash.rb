# A subclass of Hash: `[]=` raised NoMethodError at run time (#7075).
class Registry < Hash
end

r = Registry.new
r[:a] = 1
p r.size
