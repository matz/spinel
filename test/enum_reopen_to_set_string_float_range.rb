# A program's Enumerable#to_set answers a String Range and a Float Range as
# it answers an Integer one: the Set.new rewrite steps aside for every Range
# kind (CodeRabbit on #10901).
module Enumerable
  def to_set(*a) = "mine:#{first.inspect}"
end
p((1..3).to_set)
p(("a".."c").to_set)
p((1.0..2.0).to_set)
