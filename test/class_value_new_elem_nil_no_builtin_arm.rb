# A Class object read back out of an Array: the nil an element read may
# answer is no builtin Class object, so `new` on the value takes no arm for
# the builtin classes the program reads as values elsewhere (#11154).
class Store
end
class Only
  def initialize(store)
    @store = store
  end
  def self.handle?(store)
    false
  end
end
class Other
  def initialize(store)
    @store = store
  end
  def self.handle?(store)
    store.is_a?(Store)
  end
end
p [String].map(&:name)
k = [Only].first
p k.new(Store.new).class
proxies = [Only, Other]
found = proxies.find { |pr| pr.handle?(Store.new) }
p found.new(Store.new).class
e = proxies[1]
p e.new(Store.new).class
