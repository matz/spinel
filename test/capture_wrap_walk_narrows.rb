# spinel: gc-stress
# A block whose inner block reads its parameter is wrapped in a lambda called
# at once whenever a class of the program defines a yielding `each` (Bag
# here; made or not), so a proc that inner block could become captures a fresh
# cell per pass. The lambda answers the inner walk's value, its receiver: read
# as handed on, that walked Array stayed boxed. Where the wrapper's value is
# thrown away (each, times, a statement), it narrows; where something reads
# it (map, a program's own each), it stays boxed, and every answer is CRuby's.

class Bag
  def initialize(xs) = (@xs = xs)
  def each = (@xs.each { |x| yield x }; self)
end

class Neuron
  attr_accessor :v
  def initialize(v = 1.0) = (@v = v)
end

class Net
  attr_reader :hidden, :output
  def initialize(h, o)
    @hidden = Array.new(h) { |i| Neuron.new(i * 1.0) }
    @output = Array.new(o) { |i| Neuron.new(i + 0.5) }
    @pairs = 0
    @hidden.each { |s| @output.each { |d| @pairs += 1 if s.v < d.v } }
  end

  def pairs = @pairs

  # three walks deep, each inner block reading the outer parameters
  def triple(c)
    t = 0.0
    @hidden.each { |x| @output.each { |y| c.each { |z| t += x.v * y.v - z.v } } }
    t
  end

  # times and a while around the walks
  def bump(k)
    k.times { |j| @output.each { |d| d.v += j * 0.25 } }
    i = 0
    while i < 2
      @hidden.each { |s| @output.each { |d| d.v -= s.v * 0.01 * i } }
      i += 1
    end
    @output.map(&:v)
  end

  # a real proc the block makes: each pass captures its own parameter
  def procs
    ps = []
    @hidden.each { |s| ps << -> { s.v * 2 }; @output.each { |d| d.v += 0.0 } }
    ps.map(&:call)
  end
end

# Where something reads a value, the Array it answers is handed on and stays
# boxed; each case on Arrays of its own, so one cannot hide another.

# map reads the wrapper's value, the inner walk's receiver at each pass:
# @output stays boxed, @hidden (walked, not handed on) narrows
class Mapped
  def initialize(h, o)
    @hidden = Array.new(h) { |i| Neuron.new(i * 1.0) }
    @output = Array.new(o) { |i| Neuron.new(i + 0.5) }
  end
  def walked = @hidden.map { |s| @output.each { |d| d.v += s.v * 0.0 } }
  def output = @output
end

# the outer each's own value, its receiver, is read: @hidden stays boxed;
# the wrapper's value is still thrown away, so @output narrows
class OuterRead
  def initialize(h, o)
    @hidden = Array.new(h) { |i| Neuron.new(i * 1.0) }
    @output = Array.new(o) { |i| Neuron.new(i + 0.5) }
  end
  def outer_value
    r = @hidden.each { |s| @output.each { |d| d.v += s.v * 0.0 } }
    r.size
  end
end

# a program's own each may read what its block answers: @output stays boxed
class OwnEach
  def initialize(o) = (@output = Array.new(o) { |i| Neuron.new(i + 0.5) })
  def run = Bag.new([1, 2]).each { |k| @output.each { |d| d.v += k } }
  def values = @output.map(&:v)
end

net = Net.new(4, 3)
p net.pairs, net.triple([Neuron.new(0.5), Neuron.new(1.5)])
p net.bump(3)
mapped = Mapped.new(4, 3)
w = mapped.walked
p w.size, w.all? { |x| x.equal?(mapped.output) }, OuterRead.new(4, 3).outer_value, net.procs
own = OwnEach.new(3)
p own.run.class, own.values

# many nets, garbage between them
acc = 0.0
30.times do |k|
  n = Net.new(5 + k % 3, 4)
  junk = Array.new(40) { |i| "g#{i}" }
  acc += n.pairs + n.triple([Neuron.new(k * 0.1)]) + junk.size
end
p acc.round(6)
