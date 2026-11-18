# The boxed argument of method_capture_boxed_arg.rb, into a parameter the
# body stores into as a Hash. A direct call typed it with a Hash literal;
# the call of the Method hands it a boxed Hash and a boxed Integer. The
# parameter stays boxed: narrowed back to the Hash its stores describe, it
# was boxed again the next round and narrowed again, to the round cap
# (infer-test checks that the types settle). The method defined after its
# caller, and an instance method with a Symbol key, settle the same way.
def put(hash)
  hash["k"] = 1
  hash
end
def run(table, key, value) = table[key].call(value)
def late(hash)
  hash["k"] = 2
  hash
end
class Store
  def keep(hash, value)
    hash[:k] = value
    hash.size
  end
end
def refused
  p yield
rescue NoMethodError => e
  puts e.class
end
store = Store.new
p put({"k" => 0}), store.keep({a: 1}, 2)
table = {put: method(:put), late: method(:late), id: ->(value) { value }}
values = [{"k" => 5, "j" => 6}, "s", 7, {"m" => 8}]
p run(table, :put, values[0]), run(table, :late, values[3]), run(table, :id, values[1])
refused { run(table, :put, values[2]) }
refused { run(table, :late, values[2]) }
p late({"k" => 0}), put({"z" => 9})
pairs = {keep: store.method(:keep), first: ->(a, b) { a }}
def pair(table, key, a, b) = table[key].call(a, b)
kept = [{k: 5, j: 6}, nil, 3]
p pair(pairs, :keep, kept[0], kept[2]), pair(pairs, :first, kept[2], kept[1])
refused { pair(pairs, :keep, kept[1], kept[2]) }
