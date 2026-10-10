# A nil stored as a VALUE of an Integer-valued Hash (through a parameter,
# where the hash keeps its typed kind) is an entry: the statement store
# keeps it, each / each_pair / each_value bind it, a single block
# parameter gets the pair with it, values lists it, delete_if sees it, and
# `h[k] += 1` on it is the NoMethodError CRuby raises.
def put(h, k, v)
  h[k] = v
  nil
end

ints = [5, 6]
b = ints[ARGV.size + 7]
h = {"a" => 1}
put(h, "b", b)
put(h, "c", 3)
h.each { |k, v| p [k, v] }
h.each_pair { |k, v| puts "#{k}=#{v.inspect}" }
h.each_value { |v| p v }
h.each { |pr| p pr }
p h.values
p h["b"], h.key?("b")
begin
  h["b"] += 1
rescue NoMethodError => e
  puts "NoMethodError #{e.message}"
end
h["a"] += 1
h.delete_if { |k, v| v.nil? }
p h

g = {1 => 10}
put(g, 2, nil)
g.each_value { |v| p v }
p g
