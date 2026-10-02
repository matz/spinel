# each_with_index over a user Enumerable whose #each yields several values
# packs them into one element, as CRuby's Enumerable does, so the block and
# the Enumerator see [[1, 2], 0]. The builtin walked #each with one block
# parameter and kept only the first value.
class D
  include Enumerable
  def each
    yield 1, 2
    yield 3, 4
    yield 5, 6
  end
end
d = D.new
p d.each_with_index.to_a
r = d.each_with_index { |x, i| p [x, i] }
p r.class
p d.each_with_index.map { |x, i| x }
p d.each_with_index.map { |(a, b), i| a + b + i }

# one value at some sites and several at others
class Mixed
  include Enumerable
  def each
    yield 1
    yield 2, 3
    yield [4, 5]
  end
end
m = Mixed.new
p m.each_with_index.to_a
m.each_with_index { |x, i| p [x, i] }

# one value everywhere is unchanged
class Single
  include Enumerable
  def each
    yield 7
    yield 8
  end
end
s = Single.new
p s.each_with_index.to_a
p s.each_with_index { |x, i| }.class

# break, select, an external Enumerator, a Hash built in the block, and an
# #each that drives a &block
p(d.each_with_index { |(a, b), i| break [a, b, i] if i == 1 })
p d.each_with_index.select { |x, i| i.even? }
e = d.each_with_index
p e.next, e.next
h = {}
d.each_with_index { |x, i| h[i] = x }
p h
class W
  include Enumerable
  def initialize(rows) = @rows = rows
  def each(&b) = @rows.each { |k, v| b.call(k, v) }
end
p W.new([[:a, 1], [:b, 2]]).each_with_index.map { |kv, i| [kv, i] }
