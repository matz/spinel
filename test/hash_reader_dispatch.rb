# a class whose hash is a reader (`attr_reader :hash`) or a def: `x.hash`
# answers it, on a typed receiver and on a value of more than one class

class Node
  attr_reader :hash
  def initialize(h) = @hash = h
end
class Key
  def initialize(k) = @k = k
  def hash = @k * 2
end
h = 0
[1, "a", Node.new(7)].each { |x| h ^= x.hash }
p h == (1.hash ^ "a".hash ^ 7)
p [Key.new(3), :s, 2.5].map { |x| x.hash == (x.is_a?(Key) ? 6 : x.hash) }
p [Node.new(nil), 4].map(&:hash).first
p Node.new(9).hash
p 5.hash == 5.hash

class Registry
  attr_reader :hash
  def initialize = @hash = {}
end
modules = {}
entry = modules[:host] ||= Registry.new
entry.hash[:f] = 42
p entry.hash[:f]
