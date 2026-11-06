# to_r on a boxed Integer or Rational in a program whose class defines its
# own to_r: the value is never one of that class's objects, so it converts
# as the core does (#11094).
class Frac
  def initialize(n)
    @n = n
  end
  def rational_value
    @n.integer? ? Rational(@n, 2) : @n
  end
  def to_r
    rational_value.to_r
  end
end
p Rational(1).to_r
p Frac.new(Rational(1)).to_r
p Frac.new(1).to_r

def mk(f) = f ? 3 : Rational(1, 2)
p mk(true).to_r, mk(false).to_r, mk(true).rationalize, Frac.new(3).to_r
