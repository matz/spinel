# A class whose #each yields two values and that defines its own #map:
# the block takes what map yields, not the first of each's values.
class Pair
  def initialize(a, b)
    @a = a
    @b = b
  end

  def map
    [yield(@a), yield(@b)]
  end

  def flat_map
    [yield(@a), yield(@b)].flatten
  end

  def each
    yield @a, @b
  end
end

class SubPair < Pair
end

class Entries
  include Enumerable

  def each
    yield 1, :one
    yield 2, :two
  end
end

p Pair.new(1, 2).map { |x| x * 10 }
p Pair.new(3, 4).flat_map { |x| [x, x] }
p SubPair.new(5, 6).map { |x| x + 1 }
p Entries.new.map { |x| x }
