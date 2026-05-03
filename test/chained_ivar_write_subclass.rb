# Chained `@a = @b = nil` in a parent class, with concrete writes
# in a subclass that pin the slots to typed pointers (string + int).
# Without the chain-head bypass restricted to int RHS, the parent's
# nil-chain forces "nil" into the slot type each iteration of the
# inference fixpoint while the subclass's typed write cascades back
# up — the slot ping-pongs between obj_X and obj_X? and lands on
# poly, which then rejects the typed-pointer store as a C type
# error.

class Base
  def reset
    @a = @b = nil
  end
end

class Sub < Base
  def initialize
    @a = "hello"
    @b = 42
  end
  attr_reader :a, :b
end

s = Sub.new
puts s.a
puts s.b
