# A Float slot's `/=` with an Integer operand divides by the operand's
# Float, as `d / 0` does: Infinity, -Infinity or NaN for a zero, with no
# warning from the C compiler for the constant.
def local_div
  d = 1.0
  d /= 0
  d
end
p local_div

def local_neg
  d = -2.5
  d /= 0
  d
end
p local_neg

def local_nan
  d = 0.0
  d /= 0
  d.nan?
end
p local_nan

def by(n)
  d = 9.0
  d /= n
  d
end
p by(2), by(0)

class Ratio
  def initialize = (@d = 3.0)
  def half = (@d /= 2; @d)
  def blow = (@d /= 0; @d)
end
r = Ratio.new
p r.half, r.blow

$g = 4.0
$g /= 0
p $g
