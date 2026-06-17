# Ractor.shareable? / Ractor.make_shareable (#1455).
# Shareable = deeply immutable. make_shareable deep-freezes a value and returns
# it; shareable? reports the predicate.

# Immediates are always shareable.
puts Ractor.shareable?(42)
puts Ractor.shareable?(3.14)
puts Ractor.shareable?(:sym)
puts Ractor.shareable?(nil)
puts Ractor.shareable?(true)

# A frozen string is shareable; an ordinary mutable container is not.
puts Ractor.shareable?("frozen".freeze)
puts Ractor.shareable?([1, 2, 3])
puts Ractor.shareable?([1, 2, 3].freeze)

# make_shareable freezes and returns the (same-typed) value.
s = Ractor.make_shareable("hello")
puts Ractor.shareable?(s)
puts s

a = Ractor.make_shareable([10, 20, 30])
puts Ractor.shareable?(a)
puts a.sum

# Nested structure is deeply frozen.
n = Ractor.make_shareable([["a"], ["b"]])
puts Ractor.shareable?(n)

# A plain object is shareable once deeply frozen.
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end

  def x
    @x
  end
end

p = Point.new(3, 4)
puts Ractor.shareable?(p)
fp = Ractor.make_shareable(p)
puts Ractor.shareable?(fp)
puts fp.x
