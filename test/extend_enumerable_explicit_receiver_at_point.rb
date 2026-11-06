# `K.extend(Enumerable)` as a statement outside K's body (#10790): the load
# record takes it as K's extend where it stands, and the Enumerable twin
# reads the statement there too. A class method K defines before the
# extend still comes ahead of Enumerable's; one a module extended after it
# does as well, while a module extended before it stands behind.
module Firsts
  def first = :firsts
end
module Lasts
  def count = :lasts
end
class Pair
  def self.each
    yield 1, :a
    yield 2, :b
  end
  def self.min = :own_min
end
Pair.extend(Lasts)
Pair.extend(Enumerable)
Pair.extend(Firsts)
p Pair.to_a
p Pair.map { |n, s| "#{s}#{n}" }
p Pair.min
p Pair.count
p Pair.first
class Pair
  def self.max = :own_max
end
p Pair.max
p Pair.is_a?(Enumerable)
