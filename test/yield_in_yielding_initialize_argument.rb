# A `yield` written as an argument of `new` belongs to the method being emitted, not to
# the constructor spliced in for the call. `initialize` here uses block_given? or
# yield, so the call is spliced in place; its arguments used to be lowered under the
# constructor's block context.

class Probe
  def initialize(e)
    @r = block_given?
  end
  attr_reader :r
end

class Twice
  def initialize(e)
    @r = yield(e)
  end
  attr_reader :r
end

class Pair
  def initialize(a, b = 0)
    @s = a + b
    @s += yield(a) if block_given?
  end
  attr_reader :s
end

# no block on `new`: the argument's yield reached "no block given (LocalJumpError)"
def no_block
  Probe.new(yield).r
end

# a literal block on `new`: the argument's yield was bound to THAT block and answered 0
def with_block
  Twice.new(yield) { |x| x * 10 }.r
end

def two_args
  Pair.new(yield, yield) { |x| x + 1 }.s
end

def set_of
  Set.new(yield).size
end

def to_set_of
  yield.to_set.size
end

def nested
  Twice.new(Twice.new(yield) { |x| x + 1 }.r) { |x| x * 10 }.r
end

# bound first: always worked, keeps working
def bound
  v = yield
  Twice.new(v) { |x| x * 10 }.r
end

p no_block { 1 }
p with_block { 5 }
p with_block { 7 }
p two_args { 4 }
p set_of { [3, 1, 2, 3] }
p to_set_of { [3, 1, 2, 3] }
p nested { 5 }
p bound { 5 }
