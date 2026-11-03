# `&:inspect` over a receiver that reads as an Array in one round and as a
# box in the next, with a program class that has its own `map`: the block
# takes its values as a rest parameter, which the Array round lowered to a
# plain one and the next round made a rest again. The lowered body, left
# behind, still assigned the rest, so the lifted block read its own
# parameter through a cell nothing declared, and the C did not build.
class Bag
  def initialize(xs) = @xs = xs
  def map = @xs.map { |x| yield(x) }
end

class Choice
  attr_reader :allowed

  def initialize(allowed) = @allowed = allowed
  def describe(value) = "one of #{allowed.map(&:inspect).join(", ")}, got #{value.inspect}"
end

class Setter
  def set(ivar, value)
    t = @target
    t.instance_variable_set(ivar, value)
  end

  def initialize(target) = @target = target
end

module Sum
  #: (Array[Integer]) -> Integer
  def self.total(xs) = xs.sum
end

c = Choice.new([1, :b])
puts c.describe(2)
Setter.new(c).set(:"@#{"allowed"}", [[7, :z], "s"][0])
Setter.new(Bag.new([0]))
puts c.describe(3)
p Sum.total([[5, 6], "s"][0])
p Bag.new([1, 2]).map { |x| x * 2 }
p Bag.new([4, :q]).map(&:inspect)
