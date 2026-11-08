# A method the program adds to `module Comparable` answers for a Time and
# a Rational, as for a String (they are Comparable too), also when Object
# has a method of the same name. A builtin method of the receiver's class
# with the name answers first, and for a Rational a builtin Numeric method
# too (Numeric comes before Comparable in its ancestors).
class Object; def dual3 = "obj"; end
module Comparable
  def dual3 = "cmp"
  def dual4 = "c4"
  def tag(x) = "t#{x}"
  def both = [dual4, tag(1)]
  def round = "cmp round"
  def integer? = "cmp integer?"
end
puts 3.dual3, Rational(1, 2).dual3, Time.at(0).dual3
puts 2.5.dual3, "s".dual3, :s.dual3
puts Rational(1, 3).dual4, Time.at(0).dual4
p Rational(1, 3).tag(:a), Time.at(0).tag(2)
p Rational(1, 3).both, Time.at(0).both
p Rational(7, 2).round, Time.at(0).round.to_i
p Rational(1, 2).integer?, Time.at(0).integer?
p Rational(1, 2).between?(0, 1), Rational(3, 2).clamp(0, 1)
