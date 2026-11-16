# Hash.new given its keyword through `**h` takes the capacity out of it and
# raises for any other key, as Hash#initialize binds it, wherever the call
# stands; the default beside it is the Hash's default.
class Opts
  def initialize(h) = @h = h
  def to_hash = ($log << :to_hash; @h)
end
$log = []

def build(*a, **kw) = Hash.new(*a, **kw)

h = {}
x = Hash.new(**h)
p x[:a]
x = (h.size; Hash.new(7, **h))
p x[:a], x
p((h.size; Hash.new(7, **h)).fetch(:a, 9))
x = Hash.new(**{ capacity: 3 })
p x[:a]
x = Hash.new(4, **Opts.new({ capacity: 2 }))
p x[:a], $log

begin
  Hash.new(**{ k: 1 })
rescue ArgumentError => e
  p e.message
end
begin
  Hash.new(1, **Opts.new({ capacity: 1, j: 2, k: 3 }))
rescue ArgumentError => e
  p e.message
end
begin
  Hash.new(**{ capacity: "x" })
rescue TypeError => e
  p e.message
end
p build(3, capacity: 5)[:a], build(capacity: 5)[:a]
