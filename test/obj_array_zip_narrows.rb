# spinel: gc-stress
# A block zip over an Array of one class, as its receiver or its operand,
# kept it a boxed sp_PolyArray: narrow_object_arrays left every zip out, its
# emitter walking only typed scalar Arrays. The zip now walks such an Array as
# a typed one and binds its element as its sp_X *, and the Array narrows. A
# shorter operand pads with nil, as CRuby's does: an element read past it
# raises NoMethodError, a nil? answers true.

class Neuron
  attr_accessor :v
  def initialize(v = 1.0) = (@v = v)
end

class Weight
  attr_reader :w
  def initialize(w) = (@w = w)
end

class Layer
  def initialize(n)
    @cells = Array.new(n) { |i| Neuron.new(i * 1.0) }
    @weights = Array.new(n) { |i| Weight.new(0.5 + i) }
  end

  # the receiver, an operand of numbers
  def load(xs) = (@cells.zip(xs) { |c, x| c.v = x.to_f * 2 }; nil)
  # the operand, a receiver of numbers
  def dot(xs) = (s = 0.0; xs.zip(@cells) { |x, c| s += x * c.v }; s)
  # both sides, two classes
  def weighted = (s = 0.0; @cells.zip(@weights) { |c, w| s += c.v * w.w }; s)
  # one parameter: the pair
  def pairs = (out = []; @cells.zip(@weights) { |pr| out << pr[0].v + pr[1].w }; out)
  # next and break
  def skipping = (s = 0.0; @cells.zip(@weights) { |c, w| next if c.v > 4.0; s += w.w }; s)
  def until_big = (s = 0.0; @cells.zip(@weights) { |c, w| break if c.v > 2.0; s += c.v }; s)
  # a shorter operand of objects: nil past its end
  def padded(short) = (n = 0; @cells.zip(short) { |c, w| n += 1 if w.nil? }; n)
  def read_padded(short) = (s = 0.0; @cells.zip(short) { |c, w| s += w.w }; s)
  def short_weights(k) = Array.new(k) { |i| Weight.new(i * 10.0) }
  def values = @cells.map(&:v)
  # a nil in the receiver: its element read raises NoMethodError
  def add_nil = (@cells << nil; @weights << Weight.new(1.0); nil)
end

# a zip of the program's own on another class, handed the Array: its block
# takes whatever that zip yields, not the Array's element
class Pairs
  def zip(xs) = (yield 1, "one"; yield 2, xs[0]; self)
end
class Holder
  def initialize = (@cells = Array.new(3) { |i| Neuron.new(i * 1.0) })
  def kinds = (out = []; Pairs.new.zip(@cells) { |i, s| out << s.class }; out)
end
p Holder.new.kinds

l = Layer.new(4)
l.load([1, 2, 3, 4])
p l.values, l.dot([1.0, 0.5, 0.25, 0.125]), l.weighted, l.pairs, l.skipping, l.until_big
l.load([7])
p l.values
p l.padded(l.short_weights(2))
begin
  p l.read_padded(l.short_weights(1))
rescue NoMethodError => e
  p e.class
end
l.add_nil
begin
  p l.weighted
rescue NoMethodError => e
  p e.class
end
e0 = Layer.new(0)
p e0.weighted, e0.dot([]), e0.pairs

acc = 0.0
40.times do |k|
  m = Layer.new(3 + k % 4)
  junk = Array.new(30) { |i| "g#{i}" }
  m.load(Array.new(3 + k % 4) { |i| i + k })
  acc += m.weighted + m.dot([1.0, 2.0]) + junk.size
end
p acc
