# A boxed Hash whose class in the program answers last, first, sum, min
# or max -- a module the Hash class includes or prepends, or its own def --
# runs that method. The boxed dispatch sent every Hash to the builtin
# helper first: last raised NoMethodError and first answered
# Enumerable#first.

module Em
  def last = :em_last
  def first = :em_first
  def mid = :em_mid
end
class Hash
  include Em
end
p [{a: 1}, [1, 2]].map { |v| v.last }
p [{a: 1}, [1, 2]].map { |v| v.first }
p [{a: 1}, [1, 2]].map { |v| (v.mid rescue :none) }
p [{a: 1}, 7].map { |v| (v.last rescue :none) }

module Pm
  def max = :pm_max
end
class Hash
  prepend Pm
  def sum = :own_sum
end
p [{a: 1}, [1, 2]].map { |v| v.max }
p [{a: 1}, [1, 2]].map { |v| v.sum }

# a Hash subclass reaches the same methods
class SubH < Hash; end
p [SubH[b: 2], [3]].map { |v| v.first }

# a subclass's own def of the name
class OwnH < Hash; def min = :ownh_min; end
class OwnA < Array; def sum = :owna_sum; end
p [OwnH[c: 3], [5, 4]].map { |v| v.min }
p [OwnA[1, 2], [5, 4], {d: 1}].map { |v| (v.sum rescue :none) }

# max stays the builtin one for an Array: Array's own comes first
p [{a: 1}, [5, 4]].map { |v| v.max }
