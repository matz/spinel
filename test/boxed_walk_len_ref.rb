# spinel: gc-stress
# A walk over a value held boxed (each, each_with_index and the Enumerable
# methods written in Ruby) reads an Array's length where the Array keeps it
# at each step, so a block that grows or shrinks the receiver is followed;
# anything else -- a Hash, a Range, an Enumerator, a user Enumerable -- is
# walked as before.
def boxed(x) = x.respond_to?(:zzz) ? 0 : x

class Pt
  attr_reader :v
  def initialize(v) = @v = v
  def inspect = "Pt(#{@v})"
end

class Three
  include Enumerable
  def each
    yield 1
    yield 2
    yield 3
  end
end

def grow(a, extra)
  seen = []
  a.each { |x| seen << x; a << extra if a.size < 4 }
  seen
end

def shrink(a)
  seen = []
  a.each { |x| seen << x; a.pop if seen.size == 1 }
  seen
end

p grow(boxed([1, 2]), 9), grow(boxed([1.5, 2.5]), 0.5)
p grow(boxed(%w[a b]), "z"), grow(boxed([Pt.new(1)]), Pt.new(2))
p grow(boxed([1, "b"]), :c)
p shrink(boxed([1, 2, 3, 4])), shrink(boxed(%w[a b c])), shrink(boxed([1, :b, 2.0]))

c = boxed([1, 2, 3, 4])
r = []
c.each_with_index { |x, i| r << [x, i]; c.clear if i == 1 }
p r, c

e = boxed([5, 6, 7])
p e.count { |x| e << 1 if e.size < 5; x > 2 }, e

f = boxed([3, 1, 2])
p f.each_slice(2).to_a, f.min_by { |x| -x }, f.sum { |x| x * 2 }

h = boxed({ a: 1, b: 2, c: 3 })
hs = []
h.each { |k, v| hs << [k, v]; h.delete(:c) if k == :a }
p hs, h

p boxed(1..4).count(&:even?), boxed((1..3).each_slice(2)).map(&:sum)
p boxed(Three.new).select(&:odd?), boxed(Three.new).each_with_index.to_a

big = boxed(Array.new(3000) { |i| i % 5 })
s = 0
big.each { |x| s += x }
p s, big.count { |x| x > 2 }, big.any? { |x| x > 4 }
