# A method the program adds to `module Comparable` answers for a Rational
# (Rational is Comparable), also when Object has a method of the same
# name. A builtin Rational or Numeric method of the name answers first
# (Numeric comes before Comparable in a Rational's ancestors).
class Object; def dual3 = "obj"; end
module Comparable
  def dual3 = "cmp"
  def dual4 = "c4"
  def tag(x) = "t#{x}"
  def both = [dual4, tag(1)]
  def round = "cmp round"
  def integer? = "cmp integer?"
end
puts Rational(1, 2).dual3, Rational(1, 3).dual4
p Rational(1, 3).tag(:a), Rational(1, 3).both
p Rational(7, 2).round, Rational(1, 2).integer?
p Rational(1, 2).between?(0, 1), Rational(3, 2).clamp(0, 1)
