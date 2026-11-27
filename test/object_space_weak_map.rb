# ObjectSpace::WeakMap (builtins/weak_map.rb): a map keyed by object
# identity, which a connection pool's configuration registry keeps every
# instance in (`INSTANCES[self] = self` in initialize, `INSTANCES.each_key`
# to discard them all). CRuby's entries go when the collector recycles a
# key; Spinel's hold their entries, so a program reading a key it still
# holds sees the same answers. Was: "uninitialized constant WeakMap", and
# `INSTANCES[self] = self` read as an Array store.
class PoolConfig
  INSTANCES = ObjectSpace::WeakMap.new
  private_constant :INSTANCES
  attr_reader :name
  def initialize(name)
    @name = name
    INSTANCES[self] = self
  end
  def self.discard_pools! = INSTANCES.each_key(&:discard_pool!)
  def self.count = INSTANCES.size
  def self.registered?(c) = INSTANCES.key?(c)
  def discard_pool! = puts("discard #{@name}")
end
a = PoolConfig.new("a")
b = PoolConfig.new("b")
p PoolConfig.count, PoolConfig.registered?(a), PoolConfig.registered?(PoolConfig.allocate)
PoolConfig.discard_pools!

class Key
  attr_reader :n
  def initialize(n) = @n = n
  def ==(other) = other.is_a?(Key) && n == other.n   # equal by value, distinct by identity
  alias eql? ==
  def hash = n.hash
end
m = ObjectSpace::WeakMap.new
k1 = Key.new(1)
k2 = Key.new(1)
m[k1] = :first
m[k2] = :second
p m.size, m.length, m[k1], m[k2], m[Key.new(1)], m.key?(k1), m.include?(k2), m.member?(Key.new(1))
m.each { |k, v| puts "#{k.n}=#{v}" }
m.each_pair { |k, v| puts "pair #{v}" }
m.each_key { |k| p k.n }
m.each_value { |v| p v }
p m.keys.map(&:n), m.values
p m.delete(k1), m.size, m.delete(k1), m.delete(k1) { |k| "none #{k.n}" }
p m.map { |k, v| v }, m.count, m.to_a.size
