# A Set made of an Array walks the Array as CRuby's each does: a program's
# own Array#to_a was the step Set.new, to_set, merge, |, &, - and subtract
# took, so their members were what that to_a answered.
require "set"
class Array
  def to_a = map(&:to_s)
end
k = %i[dot anddot].to_set
p k.include?(:dot), k.size
s = Set.new([1, 2])
s.merge([3]); p s.include?(3)
p (s | [4]).include?(4), (s & [1]).to_a, (s - [1]).include?(1)
s.subtract([2]); p s.include?(2)
p Set.new(1..3).size
