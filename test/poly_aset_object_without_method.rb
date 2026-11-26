# `[]=` and `[]` on a boxed instance of a program class that defines neither
# raise NoMethodError, as CRuby does, whatever the key. A class that defines
# them, or method_missing, gets the call.
class Box; end

class Store
  def initialize = @h = {}
  def []=(k, v)
    @h[k] = v
  end
  def [](k) = @h[k]
end

$calls = []
class Ghost
  def method_missing(name, *args)
    $calls << [name, *args]
    [name, *args]
  end
  def respond_to_missing?(name, include_all = false) = true
end

def try
  p yield
rescue NoMethodError => e
  p [e.class, e.message]
end

box = [{}, Box.new][1]
try { box["k"] = 1 }
try { box[0] = 1 }
try { box[:k] = 1 }
try { box["k"] }
try { box[0] }
try { box[:k] }

store = [{}, Store.new][1]
store["k"] = 1
store[0] = 2
store[:k] = 3
p [store["k"], store[0], store[:k]]

ghost = [{}, Ghost.new][1]
try { ghost["k"] = 1 }
ghost[0] = 2
ghost[:s] = 3
try { ghost[0] }
p $calls

Pair = Struct.new(:a, :b)
pair = [{}, Pair.new(1, 2)][1]
pair[:a] = 3
pair["b"] = 4
pair[0] = 5
p pair.to_a

hash = [{}, Box.new][0]
hash["k"] = 1
hash[0] = 2
p hash
