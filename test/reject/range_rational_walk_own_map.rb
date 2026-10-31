# A program's own Range#map reads the end the Range holds; a Range with a
# Rational end holds it floored, so the walk is not kept for it.
class Range
  def map
    self.end
  end
end
p((1..5r / 2).map { |x| x })
