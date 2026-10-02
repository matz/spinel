# An attr_reader named like a builtin method (Hash#values / #keys), called on
# a value whose class is only known at run time, answers the reader.
class State
  attr_reader :values
  def initialize(values) = @values = values
end

class Index
  attr_reader :keys
  def initialize(keys) = @keys = keys
end

xs = [State.new([42]), 1]
p xs[0].values
ys = [Index.new([:a]), {b: 2}]
p ys[0].keys
p ys[1].keys

# the badline shape: a `return if store.nil?` guard widens the stored list
class Store
  def initialize = @states = []
  def keep(state) = @states << state
  def latest = @states.last
end

def save(store)
  return if store.nil?

  store.keep(State.new([7]))
end

store = Store.new
save(store)
p store.latest.values
