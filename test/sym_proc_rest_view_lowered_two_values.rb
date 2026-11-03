# `&:to_s` over a receiver that reads as an Array in one round and as a
# box in the next, which at run time is a program object whose `map`
# yields two values: CRuby calls `x.to_s(2)`. The Array round lowered the
# block's rest parameter to a plain one; the block must take every yielded
# value again once the receiver is a box, not only the first.
class Bag
  def initialize(xs) = @xs = xs
  def map = @xs.map { |x| yield(x, 2) }
end

class Choice
  attr_reader :allowed

  def initialize(allowed) = @allowed = allowed
  def describe(value) = "one of #{allowed.map(&:to_s).join(", ")}, got #{value.inspect}"
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

c = Choice.new([5, 6])
puts c.describe(2)
Setter.new(c).set(:"@#{"allowed"}", [[7, 9], "s"][0])
Setter.new(Bag.new([0]))
puts c.describe(3)
Setter.new(c).set(:"@#{"allowed"}", Bag.new([5, 10]))
puts c.describe(4)
p Sum.total([[5, 6], "s"][0])
p Bag.new([1, 2]).map { |x| x * 2 }
p Bag.new([4, 5]).map(&:to_s)
