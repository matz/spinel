# A `module Enumerable` reopening that has a builtin's name beside a method
# of its own: the builtin's name reaches a Hash, a Range and an Enumerator,
# and an Array keeps its own find_index. The module's includers take both.
module Enumerable
  def find_index(*a) = :e_fi
  def own_thing = :own
end

class Bag
  include Enumerable
  def each; yield 1; yield 2; end
end

p [1, 2].find_index { |x| x == 2 }
p [1, 2].find_index(2)
p({a: 1}.find_index { |x| x })
p((1..3).find_index { |x| x == 2 })
p Bag.new.find_index { |x| x == 2 }
p (1..3).own_thing, [1].own_thing, Bag.new.own_thing
