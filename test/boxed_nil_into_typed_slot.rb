# A boxed value -- a local or parameter only nil is ever written to, an
# element of a mixed Array -- assigned to an Integer or Float slot is
# unboxed, its nil landing as the slot's nil. A plain `x = v` and
# `x &&= v` did that; a multiple assignment wrote the box itself into the
# C slot and did not compile.

def and_write(x, z)
  nl = nil
  x = nil if z == 9
  x &&= nl if z.nil?
  p x
  y = (x &&= 3)
  p y
end
and_write(1, 5)
and_write(1, nil)

def and_write_float(x, z)
  nl = nil
  x = nil if z == 9
  x &&= nl if z.nil?
  p x
end
and_write_float(1.5, 5)
and_write_float(1.5, nil)

def masgn(x, z)
  nl = nil
  mk = 5
  x = nil if z == 9
  mk, x = 0, nl if z.nil?
  p [mk, x]
end
masgn(1, 5)
masgn(1, nil)

class K
  def initialize(v) = (@a = 1; @b = 2.5; @v = v)

  def swap
    @a, @b = [@v, :s][0], @v
    p [@a, @b]
  end
end
K.new(3).swap
K.new(nil).swap
