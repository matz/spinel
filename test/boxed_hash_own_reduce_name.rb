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

# min stays the builtin one: no class of the program defines it
p [{a: 1}, [5, 4]].map { |v| v.min }
