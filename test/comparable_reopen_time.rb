# A method the program adds to `module Comparable` answers for a Time, as
# for a String (both are Comparable), also when Object has a method of the
# same name. A builtin Time method of the name answers first.
class Object; def dual3 = "obj"; end
module Comparable
  def dual3 = "cmp"
  def dual4 = "c4"
  def tag(x) = "t#{x}"
  def both = [dual4, tag(1)]
  def round = "cmp round"
end
puts 3.dual3, Time.at(0).dual3, 2.5.dual3, "s".dual3, :s.dual3
puts Time.at(0).dual4
p Time.at(0).tag(2), Time.at(0).both
p Time.at(0).round.to_i
