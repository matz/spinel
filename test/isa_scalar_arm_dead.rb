# `x.is_a?(UserClass)` with a numeric receiver is statically false, so the arm
# is dead -- and it is the only place the call in it comes from. Without
# dropping it, `Int64.new(0)` (an Integer argument) made the emitter meet
# `value.value` on an Integer and refuse the program.
# (The wrap-to-signed step used `1 << 64` and `1 << 63`, literal shifts past
# int64 that the default mode now raises RangeError for like the run-time
# shift; the step was incidental, the inputs are small, so it is gone.)

class Int64
  MASK64 = 0xFFFFFFFFFFFFFFFF
  attr_reader :value

  def initialize(value = 0)
    if value.is_a?(Int64)
      @value = value.value
    else
      v = value.to_i
      @value = v & MASK64
    end
  end

  def /(other)
    b = other.value
    return Int64.new(0) if b == 0
    Int64.new(@value / b)
  end

  def %(other)
    b = other.value
    return Int64.new(0) if b == 0
    Int64.new(@value - (self / other).value * b)
  end

  def to_i
    @value
  end
end

a = Int64.new(17)
b = Int64.new(5)
p (a / b).to_i
p (a % b).to_i
p Int64.new(a).to_i
