# An Integer or Float ivar whose nil a use has tested (it raised, or it was
# not nil) reads without a test until it is written or a call could write
# it. Each case below writes nil between two uses in a way the second use
# must still see.

class Box
  def initialize(b, f)
    @b = b
    @f = f
  end

  def clear
    @b = nil
    @f = nil
  end

  # the same use twice, nothing between
  def twice
    x = @b + 1
    y = @b * 2
    [x, y]
  end

  # a method of self writes nil between the uses
  def through_self
    t = @b + 1
    clear
    @b.nil? ? [t, :nil] : [t, @b + 1]
  end

  # another object's method writes nil into self between the uses
  def through_other(other)
    t = @b + 1
    other.poke
    @b.nil? ? [t, :nil] : [t, @b + 1]
  end

  # a nil written in the loop body reaches the next iteration's use
  def loop_writes
    s = 0
    i = 0
    while i < 3
      s += @b
      @b = nil if i == 0
      i += 1
    end
    s
  end

  # a block made after the use runs after a write
  def later_block
    t = @b + 1
    pr = proc { @b.nil? }
    @b = nil
    [t, pr.call]
  end

  # a value written after a call that wrote nil clears the nil again
  def rewrite_after_call
    t = @b + 1
    clear
    @b = 3
    @b + t
  end

  # an index use tests it as well
  def index_use(a)
    x = a[@b]
    @b = nil
    [x, @b.nil?]
  end

  # a Float ivar
  def float_twice
    x = @f * 2.0
    y = @f + 0.5
    [x, y]
  end

  def float_through_self
    t = @f + 1.0
    clear
    @f.nil? ? [t, :nil] : [t, @f + 1.0]
  end

  # an operand of a comparison
  def cmp_operand
    x = 5 < @b
    y = 7 < @b
    [x, y]
  end

  attr_reader :b, :f
end

class Poker
  def initialize(box)
    @box = box
  end

  def poke
    @box.clear
  end
end

def run(label)
  p [label, yield]
rescue NoMethodError, TypeError, ArgumentError => e
  p [label, e.class]
end

run(:twice) { Box.new(3, 1.5).twice }
run(:through_self) { Box.new(3, 1.5).through_self }
run(:through_other) { b = Box.new(3, 1.5); b.through_other(Poker.new(b)) }
run(:loop_writes) { Box.new(4, 1.5).loop_writes }
run(:later_block) { Box.new(3, 1.5).later_block }
run(:rewrite_after_call) { b = Box.new(3, 1.5); [b.rewrite_after_call, b.b] }
run(:index_use) { Box.new(1, 1.5).index_use([10, 20, 30]) }
run(:float_twice) { Box.new(3, 1.5).float_twice }
run(:float_through_self) { Box.new(3, 1.5).float_through_self }
run(:nil_first) { Box.new(nil, nil).twice }
run(:nil_float) { Box.new(nil, nil).float_twice }
run(:cmp_operand) { Box.new(6, 1.5).cmp_operand }
run(:cmp_nil) { Box.new(nil, nil).cmp_operand }

# The same for a local or a parameter read from a source that can be nil.
def local_twice(a, i)
  x = a[i]
  y = x + 1
  z = x * 2
  [y, z]
end

def local_rewrite(a, i)
  x = a[i]
  y = x + 1
  x = a[i + 10]
  x.nil? ? [y, :nil] : [y, x + 1]
end

def param_twice(v)
  [v + 1, v * 2]
end

def closure_write(a)
  x = a[0]
  y = x + 1
  [1].each { x = nil }
  x.nil? ? [y, :nil] : [y, x + 1]
end

def float_param(lat, lon)
  d = lat * 2.0 + lon
  e = lat - lon
  [d, e]
end

run(:local_twice) { local_twice([1, 2, 3], 1) }
run(:local_rewrite) { local_rewrite([1, 2, 3], 0) }
run(:param_twice) { param_twice(4) }
run(:param_nil) { param_twice(nil) }
run(:closure_write) { closure_write([5]) }
run(:float_param) { float_param([1.5, 2.5][0], [1.5, 2.5][1]) }
run(:float_param_nil) { float_param([1.5][0], [1.5][3]) }
