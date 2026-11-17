# A chunk_while, slice_when, each_slice or each_cons that a `&.to_a`
# consumes, on a receiver that may be nil, answers nil for nil and the
# array of runs otherwise: on a typed Array, a boxed value, a Hash and an
# Enumerable object of the program, held in a local, assigned, returned
# from a method and two in one statement.

class Bag
  include Enumerable
  def initialize(*v) = (@v = v)
  def each(&b) = @v.each(&b)
end

def arr(f) = f ? [3, 4, 2, 5, 6] : nil
def boxed(f) = f ? [3, 4, 2] : (f.nil? ? 1 : nil)
def hsh(f) = f ? { a: 1, b: 2, c: 4 } : nil
def bag(f) = f ? Bag.new(3, 4, 2) : nil
def runs(n) = n&.chunk_while { |a, b| b == a + 1 }&.to_a

[false, true].each do |f|
  a = arr(f)
  p a&.chunk_while { |x, y| y == x + 1 }&.to_a
  p a&.to_a&.chunk_while { |x, y| y == x + 1 }&.to_a
  p a&.slice_when { |x, y| y != x + 1 }&.to_a
  p a&.each_slice(2)&.to_a
  p a&.each_cons(2)&.to_a
  p a&.each_slice(2)&.map { |s| s.sum }
  b = boxed(f)
  p b&.chunk_while { |x, y| y == x + 1 }&.to_a
  h = hsh(f)
  p h&.slice_when { |(_, x), (_, y)| y != x + 1 }&.to_a
  g = bag(f)
  p g&.chunk_while { |x, y| y == x + 1 }&.to_a
  r = a&.chunk_while { |x, y| y == x + 1 }&.to_a
  p r
  p runs(a), runs(g)
  p [a&.each_slice(3)&.to_a, a&.slice_when { |x, y| y < x }&.to_a]
  e = a&.chunk_while { |x, y| y == x + 1 }
  p e&.to_a
end
